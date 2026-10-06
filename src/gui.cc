// Dear ImGui front end.  One sovereign window: disk tree on the left, the
// action for the selected target on the right, the command's output and
// messages below, a status line at the bottom.  Every command runs on a
// worker thread (UI-IMGUI-007); the frame only reads what the worker left.
// Rules applied are cited inline by ID (docs/ui/oracle, modules/imgui.md).
#include "gui.hh"
#include "bcd.hh"
#include "commands.hh"
#include "disk.hh"
#include "log.hh"
#include "version.hh"

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include <d3d11.h>
#include <shlobj.h>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

// ---------------------------------------------------------------- strings
std::string utf8(const std::wstring& w)
{
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, NULL, NULL);
    return s;
}

std::wstring wide(const std::string& s)
{
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

std::wstring settingsDir()
{
    wchar_t* p = nullptr;
    std::wstring d;
    if (SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, NULL, &p) == S_OK) { d = p; CoTaskMemFree(p); }
    d += L"\\bootfix";
    CreateDirectoryW(d.c_str(), NULL);
    return d;
}

// ---------------------------------------------------------------- settings (UI-ARCH-002, UI-IMGUI-011/015)
struct Settings {
    float textScale = 1.0f;   // style.FontScaleMain, 0.8 .. 2.25
    int theme = 0;            // 0 = follow Windows, 1 = light, 2 = dark
};

Settings loadSettings()
{
    Settings s;
    FILE* f = _wfopen((settingsDir() + L"\\gui.ini").c_str(), L"r");
    if (!f) return s;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        float v;
        int i;
        if (sscanf_s(line, "textScale=%f", &v) == 1 && v >= 0.8f && v <= 2.25f) s.textScale = v;
        if (sscanf_s(line, "theme=%d", &i) == 1 && i >= 0 && i <= 2) s.theme = i;
    }
    fclose(f);
    return s;
}

void saveSettings(const Settings& s)
{
    FILE* f = _wfopen((settingsDir() + L"\\gui.ini").c_str(), L"w");
    if (!f) return;
    fprintf(f, "textScale=%.2f\ntheme=%d\n", s.textScale, s.theme);
    fclose(f);
}

bool windowsUsesLightTheme()
{
    DWORD v = 1, len = sizeof v;
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0, KEY_READ, &k) != ERROR_SUCCESS)
        return true;
    RegQueryValueExW(k, L"AppsUseLightTheme", NULL, NULL, reinterpret_cast<BYTE*>(&v), &len);
    RegCloseKey(k);
    return v != 0;
}

// ---------------------------------------------------------------- palette (UI-COLOR-005, UI-IMGUI-013/014)
struct Palette {
    ImVec4 error, warning, success, muted;
};
Palette g_pal;

void applyTheme(bool dark)
{
    ImGuiStyle& s = ImGui::GetStyle();
    if (dark) ImGui::StyleColorsDark(&s); else ImGui::StyleColorsLight(&s);
    // Text at full contrast in both palettes; the navigation cursor distinct from hover/active/header (UI-IMGUI-003).
    s.Colors[ImGuiCol_Text]         = dark ? ImVec4(0.95f, 0.95f, 0.95f, 1) : ImVec4(0.08f, 0.08f, 0.08f, 1);
    s.Colors[ImGuiCol_TextDisabled] = dark ? ImVec4(0.65f, 0.65f, 0.65f, 1) : ImVec4(0.38f, 0.38f, 0.38f, 1);
    s.Colors[ImGuiCol_NavCursor]    = dark ? ImVec4(1.00f, 0.85f, 0.20f, 1) : ImVec4(0.00f, 0.30f, 0.90f, 1);
    s.Colors[ImGuiCol_WindowBg]     = dark ? ImVec4(0.11f, 0.11f, 0.12f, 1) : ImVec4(0.97f, 0.97f, 0.97f, 1);
    if (dark) { g_pal.error = ImVec4(1.0f, 0.45f, 0.45f, 1); g_pal.warning = ImVec4(1.0f, 0.80f, 0.30f, 1); g_pal.success = ImVec4(0.45f, 0.90f, 0.50f, 1); g_pal.muted = s.Colors[ImGuiCol_TextDisabled]; }
    else      { g_pal.error = ImVec4(0.72f, 0.05f, 0.05f, 1); g_pal.warning = ImVec4(0.55f, 0.35f, 0.00f, 1); g_pal.success = ImVec4(0.00f, 0.45f, 0.10f, 1); g_pal.muted = s.Colors[ImGuiCol_TextDisabled]; }
}

// ---------------------------------------------------------------- state shared with the worker
struct Line { int level; std::string text; };

std::mutex g_mx;
std::vector<Line> g_messages;          // stderr-class lines, level 0..4
std::string g_output;                  // stdout-class text (product)
std::string g_status;                  // last error/warning/info for the status line
int g_statusLevel = 2;
std::vector<DiskInfo> g_disks;         // from the last scan
bool g_disksFresh = false;

std::atomic<bool> g_busy(false);
std::atomic<int> g_lastRc(0);
std::string g_busyWhat;
std::thread g_worker;

void sink(int level, const std::wstring& line)
{
    std::lock_guard<std::mutex> lk(g_mx);
    if (level < 0) { g_output += utf8(line); g_output += '\n'; return; }
    if (level <= LOG_LEVEL_INFO) {
        g_messages.push_back(Line{ level, utf8(line) });
        g_status = utf8(line);
        g_statusLevel = level;
    }
}

void startJob(const std::string& what, std::function<int()> fn)
{
    if (g_busy) return;
    if (g_worker.joinable()) g_worker.join();
    g_busy = true;
    g_busyWhat = what;
    { std::lock_guard<std::mutex> lk(g_mx); g_output.clear(); }
    g_worker = std::thread([fn]() {
        int rc = EXIT_FAILED;
        try { rc = fn(); } catch (const std::wstring& e) { LOG_ERROR(L"%s", e.c_str()); }
        g_lastRc = rc;
        g_busy = false;
    });
}

// Re-read the disk layout for the tree (the same enumeration scan uses).
int jobEnumerate()
{
    std::vector<DiskInfo> d;
    disk::enumerate(&d);
    std::lock_guard<std::mutex> lk(g_mx);
    g_disks.swap(d);
    g_disksFresh = true;
    return EXIT_OK;
}

// ---------------------------------------------------------------- UI state
struct Ui {
    Settings settings;
    bool dark = false;
    int selDisk = -1, selPart = -1;
    bool dryRun = true;
    bool nt52 = false, fromBackup = false, dismount = false, recreate = false;
    int firmware = 0;     // 0 auto, 1 UEFI, 2 BIOS
    int live = 1;         // 1 = the live system store, 0 = a BCD file
    char volume[260] = "";
    char esp[260] = "";
    char windir[520] = "";
    char store[520] = "";
    char entry[64] = "{bootmgr}";
    char assignment[260] = "";
    // confirmation (UI-IMGUI-006, UI-DLG-005, UI-TEXT-012)
    std::string question, verb;
    std::vector<std::string> details;
    std::function<int()> pendingRun;
    bool openConfirm = false;
};
Ui ui;

Args baseArgs(const char* command, const char* verb = nullptr)
{
    Args a;
    a.command = wide(command);
    if (verb) a.verb = wide(verb);
    a.options[L"force"] = L"";   // the GUI asked already
    if (ui.dryRun) a.options[L"dry-run"] = L"";
    return a;
}

void runOrConfirm(const std::string& question, const std::string& verb, const std::vector<std::string>& details, std::function<int()> fn, const std::string& what)
{
    if (ui.dryRun) { startJob(what, fn); return; }
    ui.question = question;
    ui.verb = verb;
    ui.details = details;
    ui.pendingRun = fn;
    ui.openConfirm = true;
}

float fs() { return ImGui::GetFontSize(); }

// ---------------------------------------------------------------- panes
void drawDiskTree()
{
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_disks.empty()) {
        ImGui::TextWrapped("%s", g_disksFresh ? "No disk could be opened. Run bootfix from an administrator prompt." : "Press Scan disks (F5) to read the attached disks.");
        return;
    }
    for (size_t i = 0; i < g_disks.size(); i++) {
        const DiskInfo& d = g_disks[i];
        ImGui::PushID((int)i);
        char label[256];
        snprintf(label, sizeof label, "Disk %d: %s %s, %llu MB, %s", d.number, utf8(d.busType).c_str(), utf8(d.model).c_str(),
                 (unsigned long long)(d.sizeBytes >> 20), d.gpt ? "GPT" : "MBR");
        ImGuiTreeNodeFlags tf = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
        if ((int)i == ui.selDisk && ui.selPart < 0) tf |= ImGuiTreeNodeFlags_Selected;
        bool open = ImGui::TreeNodeEx("disk", tf, "%s", label);
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) { ui.selDisk = (int)i; ui.selPart = -1; }
        if (open) {
            for (size_t k = 0; k < d.partitions.size(); k++) {
                const PartitionInfo& p = d.partitions[k];
                ImGui::PushID((int)k);
                std::string type = utf8(p.gpt ? disk::gptTypeName(p.gptType) : disk::mbrTypeName(p.mbrType));
                snprintf(label, sizeof label, "Partition %d: %s, %llu MB, %s, %s%s", p.number, type.c_str(), (unsigned long long)(p.sizeBytes >> 20),
                         p.driveLetter.empty() ? "no letter" : utf8(p.driveLetter).c_str(),
                         p.fileSystem.empty() ? "no file system" : utf8(p.fileSystem).c_str(),
                         p.mbrActive ? ", active" : p.isEsp() ? ", ESP" : "");
                bool sel = (int)i == ui.selDisk && (int)k == ui.selPart;
                if (ImGui::Selectable(label, sel)) {
                    ui.selDisk = (int)i; ui.selPart = (int)k;
                    std::string v = utf8(p.driveLetter.empty() ? p.ntDevice : p.driveLetter);
                    strncpy_s(ui.volume, v.c_str(), _TRUNCATE);
                    if (p.isEsp()) strncpy_s(ui.esp, v.c_str(), _TRUNCATE);
                }
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
}

void drawScanTab()
{
    ImGui::TextWrapped("Reads every disk: partition tables, boot sectors and their backup copies, boot files and Windows installations. Writes nothing.");
    ImGui::Spacing();
    ImGui::BeginDisabled(g_busy);
    if (ImGui::Button("Scan disks")) {
        startJob("scan", []() { jobEnumerate(); Args a = baseArgs("scan"); a.options.erase(L"dry-run"); return cmdScan(a); });
    }
    ImGui::EndDisabled();
    if (g_busy) ImGui::SetItemTooltip("Wait for the current job to finish");
    ImGui::SameLine();
    ImGui::TextDisabled("F5");
}

void drawFixMbrTab()
{
    std::string diskName = "none selected";
    int diskNo = -1;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        if (ui.selDisk >= 0 && ui.selDisk < (int)g_disks.size()) { diskNo = g_disks[ui.selDisk].number; diskName = "disk " + std::to_string(diskNo) + " (" + utf8(g_disks[ui.selDisk].model) + ")"; }
    }
    ImGui::TextWrapped("Writes the 440 bytes of Windows MBR boot code to sector 0 of the selected disk (bootrec /FixMbr). The disk signature and partition table are kept; the old sector is saved first.");
    ImGui::Spacing();
    ImGui::Text("Target: %s", diskName.c_str());
    ImGui::Checkbox("NTLDR-compatible code (Windows XP/2003)##mbr", &ui.nt52);
    ImGui::Spacing();
    ImGui::BeginDisabled(g_busy || diskNo < 0);
    if (ImGui::Button(ui.dryRun ? "Show what fixmbr would write" : "Write MBR boot code...")) {
        int n = diskNo; bool nt52 = ui.nt52;
        runOrConfirm("Write " + std::string(nt52 ? "NTLDR" : "BOOTMGR") + " boot code to the MBR of " + diskName + "?", "Write boot code",
                     { "Sector 0 of the disk is rewritten; the partition table and disk signature stay.", "The old sector is saved in the backup directory first." },
                     [n, nt52]() { Args a = baseArgs("fixmbr"); a.options[L"disk"] = std::to_wstring(n); if (nt52) a.options[L"nt52"] = L""; return cmdFixMbr(a); },
                     "fixmbr");
    }
    ImGui::EndDisabled();
    if (diskNo < 0) ImGui::SetItemTooltip("Select a disk in the list on the left");
}

void drawFixBootTab()
{
    ImGui::TextWrapped("Writes the Windows boot code for the volume's file system (FAT, FAT32, NTFS, exFAT) like bootsect /nt60, keeping the BPB. When sector 0 is unreadable, or with the option below, the backup boot sector is restored first like bootrec /FixBoot.");
    ImGui::Spacing();
    ImGui::SetNextItemWidth(fs() * 20);
    ImGui::InputText("Volume (letter or \\Device\\HarddiskVolumeN)##vol", ui.volume, sizeof ui.volume);
    ImGui::Checkbox("NTLDR-compatible code (Windows XP/2003)##vbr", &ui.nt52);
    ImGui::Checkbox("Restore sector 0 from the backup boot sector first", &ui.fromBackup);
    ImGui::Checkbox("Dismount even if the volume cannot be locked", &ui.dismount);
    ImGui::SetItemTooltip("bootsect /force: programs with files open on the volume lose them");
    ImGui::Spacing();
    bool haveVol = ui.volume[0] != 0;
    ImGui::BeginDisabled(g_busy || !haveVol);
    if (ImGui::Button(ui.dryRun ? "Show what fixboot would write" : "Write volume boot code...")) {
        std::string vol = ui.volume; bool nt52 = ui.nt52, fb = ui.fromBackup, dm = ui.dismount;
        runOrConfirm("Write " + std::string(nt52 ? "NTLDR" : "BOOTMGR") + " boot code to " + vol + "?", "Write boot code",
                     { "The boot sectors of the volume are rewritten; the BPB (volume geometry) stays.", "The old sectors are saved in the backup directory first." },
                     [vol, nt52, fb, dm]() { Args a = baseArgs("fixboot"); a.operands.push_back(wide(vol)); if (nt52) a.options[L"nt52"] = L""; if (fb) a.options[L"from-backup"] = L""; if (dm) a.options[L"dismount"] = L""; return cmdFixBoot(a); },
                     "fixboot");
    }
    ImGui::EndDisabled();
    if (!haveVol) ImGui::SetItemTooltip("Enter a volume or select a partition on the left");
}

void drawBcdTab()
{
    ImGui::Text("Store");
    ImGui::RadioButton("This machine's live store", &ui.live, 1);
    ImGui::SameLine();
    ImGui::RadioButton("BCD file:", &ui.live, 0);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(fs() * 22);
    ImGui::InputText("##storepath", ui.store, sizeof ui.store);
    ImGui::Separator();

    ImGui::BeginDisabled(g_busy);
    if (ImGui::Button("List objects and elements")) {
        bool live = ui.live != 0; std::string st = ui.store;
        startJob("bcd dump", [live, st]() { Args a = baseArgs("bcd", "dump"); a.options.erase(L"dry-run"); if (live) a.options[L"live"] = L""; else a.options[L"store"] = wide(st); return cmdBcd(a); });
    }
    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::Text("Set an element");
    ImGui::SetNextItemWidth(fs() * 14);
    ImGui::InputText("Entry##entry", ui.entry, sizeof ui.entry);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(fs() * 18);
    ImGui::InputText("name=value##assign", ui.assignment, sizeof ui.assignment);
    ImGui::SetItemTooltip("Examples: timeout=5  description=Windows 10  osdevice=partition=C:  displayorder={a},{b}");
    ImGui::BeginDisabled(g_busy || ui.assignment[0] == 0);
    if (ImGui::Button(ui.dryRun ? "Show what set would change" : "Set element...")) {
        bool live = ui.live != 0; std::string st = ui.store, en = ui.entry, as = ui.assignment;
        runOrConfirm("Set " + as + " on " + en + "?", "Set element", { "The store is changed in place; export it first if you want a copy." },
                     [live, st, en, as]() { Args a = baseArgs("bcd", "set"); if (live) a.options[L"live"] = L""; else a.options[L"store"] = wide(st); a.options[L"entry"] = wide(en); a.operands.push_back(wide(as)); return cmdBcd(a); },
                     "bcd set");
    }
    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text("Rebuild (bootrec /RebuildBcd + bcdboot)");
    ImGui::SetNextItemWidth(fs() * 22);
    ImGui::InputText("Windows directory (optional, e.g. F:\\Windows)##windir", ui.windir, sizeof ui.windir);
    ImGui::SetNextItemWidth(fs() * 22);
    ImGui::InputText("System partition / ESP (optional)##esp", ui.esp, sizeof ui.esp);
    ImGui::SetNextItemWidth(fs() * 10);
    ImGui::Combo("Firmware##fw", &ui.firmware, "from the disk layout\0UEFI\0BIOS\0");
    ImGui::Checkbox("Start the store over from BCD-Template", &ui.recreate);
    ImGui::BeginDisabled(g_busy);
    if (ImGui::Button(ui.dryRun ? "Show what rebuild would do" : "Rebuild boot files and BCD...")) {
        std::string wd = ui.windir, esp = ui.esp; int fw = ui.firmware; bool rc = ui.recreate;
        runOrConfirm("Copy the boot files to the system partition and update its BCD?", "Rebuild",
                     { "Files under EFI\\Microsoft\\Boot (or \\Boot) on the system partition are replaced.", rc ? "The BCD is recreated from BCD-Template; the old one is saved first." : "Missing boot entries are added to the existing BCD; it is exported first." },
                     [wd, esp, fw, rc]() { Args a = baseArgs("bcd", "rebuild"); if (!wd.empty()) a.operands.push_back(wide(wd)); if (!esp.empty()) a.options[L"esp"] = wide(esp); if (fw == 1) a.options[L"firmware"] = L"UEFI"; if (fw == 2) a.options[L"firmware"] = L"BIOS"; if (rc) a.options[L"recreate"] = L""; return cmdBcd(a); },
                     "bcd rebuild");
    }
    ImGui::EndDisabled();
}

void drawOutputPanes()
{
    if (ImGui::BeginTabBar("##bottom")) {
        if (ImGui::BeginTabItem("Output")) {
            std::lock_guard<std::mutex> lk(g_mx);
            // Read-only, selectable, copyable (UI-IMGUI-018).
            ImGui::InputTextMultiline("##out", &g_output[0], g_output.size() + 1, ImVec2(-1, -1), ImGuiInputTextFlags_ReadOnly);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Messages")) {
            {
                std::lock_guard<std::mutex> lk(g_mx);
                if (ImGui::Button("Copy all")) {
                    std::string all;
                    for (const Line& l : g_messages) all += (l.level == 0 ? "error: " : l.level == 1 ? "warning: " : "") + l.text + "\n";
                    ImGui::SetClipboardText(all.c_str());
                }
                ImGui::SameLine();
                if (ImGui::Button("Clear")) g_messages.clear();
            }
            ImGui::BeginChild("##msgs", ImVec2(0, 0), ImGuiChildFlags_Borders);
            std::lock_guard<std::mutex> lk(g_mx);
            for (size_t i = 0; i < g_messages.size(); i++) {
                const Line& l = g_messages[i];
                ImGui::PushID((int)i);
                // Word and colour together, never colour alone (UI-COLOR-003).
                if (l.level == 0) { ImGui::TextColored(g_pal.error, "error:"); ImGui::SameLine(); }
                else if (l.level == 1) { ImGui::TextColored(g_pal.warning, "warning:"); ImGui::SameLine(); }
                ImGui::TextWrapped("%s", l.text.c_str());
                ImGui::PopID();
            }
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

void drawConfirmModal()
{
    if (ui.openConfirm) { ImGui::OpenPopup("Confirm##modal"); ui.openConfirm = false; }
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(fs() * 32, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Confirm##modal", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.15f);
        ImGui::TextWrapped("%s", ui.question.c_str());
        ImGui::PopFont();
        ImGui::Spacing();
        for (const std::string& d : ui.details) ImGui::BulletText("%s", d.c_str());
        ImGui::Spacing();
        // Cancel is the default item: Enter and Escape both leave without writing (UI-IMGUI-006, UI-DLG-005).
        bool cancel = ImGui::Button("Cancel", ImVec2(fs() * 8, 0));
        ImGui::SetItemDefaultFocus();
        ImGui::SameLine();
        bool go = ImGui::Button(ui.verb.c_str(), ImVec2(fs() * 12, 0));
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) cancel = true;
        if (cancel) { ui.pendingRun = nullptr; ImGui::CloseCurrentPopup(); }
        if (go && ui.pendingRun) { std::function<int()> fn = ui.pendingRun; ui.pendingRun = nullptr; ImGui::CloseCurrentPopup(); startJob(ui.verb, fn); }
        ImGui::EndPopup();
    }
}

void drawFrame(bool* wantQuit)
{
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGuiWindowFlags wf = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin("bootfix##main", nullptr, wf);

    // Shortcut F5 is bound here and shown in the menu (UI-IMGUI-017).
    if (ImGui::Shortcut(ImGuiKey_F5, ImGuiInputFlags_RouteGlobal) && !g_busy)
        startJob("scan", []() { jobEnumerate(); Args a = baseArgs("scan"); a.options.erase(L"dry-run"); return cmdScan(a); });

    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Scan disks", "F5", false, !g_busy))
                startJob("scan", []() { jobEnumerate(); Args a = baseArgs("scan"); a.options.erase(L"dry-run"); return cmdScan(a); });
            ImGui::Separator();
            if (ImGui::MenuItem("Exit")) *wantQuit = true;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            ImGui::SetNextItemWidth(fs() * 10);
            if (ImGui::SliderFloat("Text scale", &ui.settings.textScale, 0.8f, 2.25f, "%.2fx")) {
                ImGui::GetStyle().FontScaleMain = ui.settings.textScale;   // UI-IMGUI-011
                saveSettings(ui.settings);
            }
            bool changed = false;
            changed |= ImGui::RadioButton("Follow Windows theme", &ui.settings.theme, 0);
            changed |= ImGui::RadioButton("Light", &ui.settings.theme, 1);
            changed |= ImGui::RadioButton("Dark", &ui.settings.theme, 2);
            if (changed) {
                ui.dark = ui.settings.theme == 2 || (ui.settings.theme == 0 && !windowsUsesLightTheme());
                applyTheme(ui.dark);
                saveSettings(ui.settings);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            ImGui::Text("bootfix %s", utf8(BOOTFIX_VERSION).c_str());
            ImGui::TextDisabled("Log: %%LOCALAPPDATA%%\\bootfix\\bootfix.log");
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    if (!isElevated()) {
        ImGui::TextColored(g_pal.warning, "warning:");
        ImGui::SameLine();
        ImGui::TextWrapped("not running as administrator: disks cannot be read and nothing can be written. Start bootfix from an administrator prompt.");
        ImGui::Separator();
    }

    float statusH = ImGui::GetFrameHeightWithSpacing();
    float bodyH = ImGui::GetContentRegionAvail().y - statusH;
    ImGui::BeginChild("##left", ImVec2(fs() * 24, bodyH), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
    ImGui::Text("Disks");
    ImGui::Separator();
    drawDiskTree();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##right", ImVec2(0, bodyH));
    {
        ImGui::BeginChild("##actions", ImVec2(0, fs() * 19), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeY);
        ImGui::Checkbox("Dry run: show what would be written, write nothing", &ui.dryRun);
        ImGui::Separator();
        if (ImGui::BeginTabBar("##actiontabs")) {
            if (ImGui::BeginTabItem("Scan")) { drawScanTab(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Fix MBR")) { drawFixMbrTab(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Fix boot code")) { drawFixBootTab(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("BCD")) { drawBcdTab(); ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
        ImGui::EndChild();
        ImGui::BeginChild("##bottompane", ImVec2(0, 0), ImGuiChildFlags_Borders);
        drawOutputPanes();
        ImGui::EndChild();
    }
    ImGui::EndChild();

    // Status line: busy indicator with motion so a stall is distinguishable (UI-FB-004), last message with its kind.
    ImGui::Separator();
    if (g_busy) {
        const char* spin = "|/-\\";
        ImGui::Text("%c Running %s...", spin[(int)(ImGui::GetTime() * 6) % 4], g_busyWhat.c_str());
    } else {
        std::lock_guard<std::mutex> lk(g_mx);
        if (g_statusLevel == 0) { ImGui::TextColored(g_pal.error, "error:"); ImGui::SameLine(); }
        else if (g_statusLevel == 1) { ImGui::TextColored(g_pal.warning, "warning:"); ImGui::SameLine(); }
        ImGui::TextUnformatted(g_status.empty() ? "Ready" : g_status.c_str());
    }
    drawConfirmModal();
    ImGui::End();
}

// ---------------------------------------------------------------- Direct3D / Win32 host
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swap = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
UINT g_resizeW = 0, g_resizeH = 0;

void createRenderTarget()
{
    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(g_swap->GetBuffer(0, IID_PPV_ARGS(&back))) && back) {
        g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}

void cleanupRenderTarget()
{
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
}

bool createDevice(HWND hwnd)
{
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd, &g_swap, &g_device, &got, &g_context);
    if (hr == DXGI_ERROR_UNSUPPORTED)
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd, &g_swap, &g_device, &got, &g_context);
    if (FAILED(hr)) return false;
    createRenderTarget();
    return true;
}

void cleanupDevice()
{
    cleanupRenderTarget();
    if (g_swap) { g_swap->Release(); g_swap = nullptr; }
    if (g_context) { g_context->Release(); g_context = nullptr; }
    if (g_device) { g_device->Release(); g_device = nullptr; }
}

LRESULT WINAPI wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
    switch (msg) {
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) { g_resizeW = LOWORD(lp); g_resizeH = HIWORD(lp); }
        return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xfff0) == SC_KEYMENU) return 0;  // Alt alone does not open a menu
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int runGui(const Args&)
{
    ImGui_ImplWin32_EnableDpiAwareness();   // UI-IMGUI-009
    WNDCLASSEXW wc = { sizeof wc, CS_CLASSDC, wndProc, 0, 0, GetModuleHandleW(nullptr), nullptr, LoadCursor(nullptr, IDC_ARROW), nullptr, nullptr, L"bootfix", nullptr };
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"bootfix", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1100, 720, nullptr, nullptr, wc.hInstance, nullptr);
    if (!createDevice(hwnd)) {
        LOG_ERROR(L"Direct3D 11 is not available on this machine; the graphical interface cannot start. The commands work without it");
        cleanupDevice();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return EXIT_FAILED;
    }
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;       // UI-IMGUI-002
    io.ConfigWindowsMoveFromTitleBarOnly = true;                // UI-IMGUI-016
    static std::string iniPath = utf8(settingsDir() + L"\\imgui.ini");
    io.IniFilename = iniPath.c_str();                           // UI-IMGUI-015
#ifdef NDEBUG
    io.ConfigErrorRecoveryEnableAssert = false;                 // UI-IMGUI-021: toolkit diagnostics to the log, not the operator
    io.ConfigErrorRecoveryEnableTooltip = false;
    io.ConfigErrorRecoveryEnableDebugLog = true;
#endif
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    ImGuiStyle& style = ImGui::GetStyle();
    float dpi = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);
    style.FontSizeBase = 16.0f;
    style.FontScaleDpi = dpi;                                   // UI-IMGUI-009
    style.ScaleAllSizes(dpi);
    ui.settings = loadSettings();
    style.FontScaleMain = ui.settings.textScale;
    ui.dark = ui.settings.theme == 2 || (ui.settings.theme == 0 && !windowsUsesLightTheme());
    applyTheme(ui.dark);                                        // UI-IMGUI-013

    // Scalable system font (UI-IMGUI-010); the embedded bitmap font only as a logged fallback.
    if (!io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf"))
        LOG_WARN(L"C:\\Windows\\Fonts\\segoeui.ttf is missing; the built-in bitmap font is used and text will be blurry when scaled");

    logging::setSink(sink);
    LOG_INFO(L"graphical interface started; the log file is in %s", settingsDir().c_str());
    startJob("scan", []() { return jobEnumerate(); });

    bool done = false, wantQuit = false;
    while (!done) {
        // Idle at a few frames per second; interactive rate only while input arrives or a job runs (UI-IMGUI-008).
        MsgWaitForMultipleObjects(0, nullptr, FALSE, g_busy ? 16 : 250, QS_ALLINPUT);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;
        if (wantQuit) { DestroyWindow(hwnd); continue; }
        if (g_resizeW && g_resizeH) {
            cleanupRenderTarget();
            g_swap->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0);
            g_resizeW = g_resizeH = 0;
            createRenderTarget();
        }
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        drawFrame(&wantQuit);
        ImGui::Render();
        const float clear[4] = { ui.dark ? 0.11f : 0.97f, ui.dark ? 0.11f : 0.97f, ui.dark ? 0.12f : 0.97f, 1.0f };
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_context->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swap->Present(1, 0);
    }

    if (g_worker.joinable()) g_worker.join();
    logging::setSink(nullptr);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    cleanupDevice();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return g_lastRc == EXIT_OK ? EXIT_OK : EXIT_FAILED;
}
