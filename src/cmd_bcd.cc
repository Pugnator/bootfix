// `bcd <verb>`:
//   dump     bcdedit /enum all /v on any store file, devices decoded, --json
//   rebuild  bootrec /RebuildBcd + bcdboot: boot files + missing entries
//   set, unset, create, delete, export   bcdedit's editing verbs
#include "bcd.hh"
#include "commands.hh"
#include "disk.hh"
#include "json.hh"
#include "log.hh"
#include "ntapi.hh"
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>

bool openStoreFromArgs(const Args& args, bool readOnly, bcd::Store* store)
{
    std::wstring path = args.get(L"store");
    if (path.empty() && !args.has(L"live")) {
        LOG_ERROR(L"say which store: --store FILE (a BCD file) or --live (this machine's mounted system store)");
        return false;
    }
    if (!requireElevation(L"bcd")) return false;
    if (!path.empty()) return store->openFile(path, readOnly);
    return store->openLiveSystem(readOnly);
}

namespace {

// Exit code after openStoreFromArgs failed: usage when no store was named, elevation when not elevated.
int storeExit(const Args& args)
{
    if (args.get(L"store").empty() && !args.has(L"live")) return EXIT_USAGE;
    return isElevated() ? EXIT_FAILED : EXIT_ELEVATION;
}

struct Volume {
    const DiskInfo* disk;
    const PartitionInfo* part;
    std::wstring root;   // "C:\" or "\\?\GLOBALROOT\Device\HarddiskVolumeN\"
};

std::wstring rootFor(const PartitionInfo& p)
{
    if (!p.driveLetter.empty()) return p.driveLetter + L"\\";
    if (!p.ntDevice.empty()) return nt::globalRoot(p.ntDevice) + L"\\";
    if (!p.volumeGuidPath.empty()) return p.volumeGuidPath;
    return L"";
}

bool exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
bool isDir(const std::wstring& p) { DWORD a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }

std::vector<Volume> allVolumes(const std::vector<DiskInfo>& disks)
{
    std::vector<Volume> v;
    for (const DiskInfo& d : disks)
        for (const PartitionInfo& p : d.partitions) {
            std::wstring r = rootFor(p);
            if (!r.empty()) v.push_back(Volume{ &d, &p, r });
        }
    return v;
}

const Volume* findVolume(const std::vector<Volume>& vols, const std::vector<DiskInfo>& disks, const std::wstring& spec)
{
    const DiskInfo* d; const PartitionInfo* p;
    if (!disk::findPartition(disks, spec, &d, &p)) return nullptr;
    for (const Volume& v : vols) if (v.part == p) return &v;
    return nullptr;
}

bool copyFileLogged(const std::wstring& from, const std::wstring& to, bool dryRun)
{
    if (dryRun) { LOG_INFO(L"dry run: copy %s -> %s", from.c_str(), to.c_str()); return true; }
    SetFileAttributesW(to.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (!CopyFileW(from.c_str(), to.c_str(), FALSE)) {
        DWORD e = GetLastError();
        LOG_ERROR(L"cannot copy %s to %s: %s", from.c_str(), to.c_str(), logging::win32Error(e).c_str());
        return false;
    }
    LOG_DEBUG(L"copied %s -> %s", from.c_str(), to.c_str());
    return true;
}

bool makeDir(const std::wstring& p, bool dryRun)
{
    if (isDir(p)) return true;
    if (dryRun) { LOG_INFO(L"dry run: mkdir %s", p.c_str()); return true; }
    size_t start = p.find(L"HarddiskVolume");
    size_t pos = p.find(L'\\', start == std::wstring::npos ? 3 : p.find(L'\\', start) + 1);
    while (pos != std::wstring::npos) {
        CreateDirectoryW(p.substr(0, pos).c_str(), NULL);
        pos = p.find(L'\\', pos + 1);
    }
    if (!CreateDirectoryW(p.c_str(), NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        LOG_ERROR(L"cannot create %s: %s", p.c_str(), logging::win32Error(GetLastError()).c_str());
        return false;
    }
    return true;
}

// Recursive copy (bcdboot: BfsServiceBootFilesEx copies |SOURCE|\|FWTYPE|, Fonts, Resources, <lang>\*.mui).
int copyTree(const std::wstring& from, const std::wstring& to, bool dryRun, int* failures)
{
    int copied = 0;
    if (!makeDir(to, dryRun)) { ++*failures; return 0; }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((from + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        std::wstring s = from + L"\\" + fd.cFileName, d = to + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            copied += copyTree(s, d, dryRun, failures);
        else if (copyFileLogged(s, d, dryRun)) copied++;
        else ++*failures;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return copied;
}

std::wstring elementValueText(const bcd::Element& e, DWORD objType)
{
    std::wstring s = bcd::describeElement(e, objType);
    size_t sp = s.find(L' ');
    while (sp != std::wstring::npos && s[sp] == L' ') sp++;
    return sp == std::wstring::npos ? L"" : s.substr(sp);
}

void dumpStore(bcd::Store& store, bool json)
{
    std::vector<bcd::Object> objs;
    if (!store.listObjects(&objs)) return;
    if (json) {
        JsonWriter j;
        j.beginObject();
        j.kv(L"store", store.filePath());
        j.key(L"objects"); j.beginArray();
        for (const bcd::Object& o : objs) {
            j.beginObject();
            j.kv(L"id", bcd::guidString(o.id)); j.kv(L"alias", bcd::objectAlias(o.id));
            wchar_t t[16]; _snwprintf_s(t, 16, _TRUNCATE, L"0x%08X", o.type);
            j.kv(L"type", t); j.kv(L"typeName", bcd::objectTypeName(o.type));
            j.key(L"elements"); j.beginArray();
            for (const bcd::Element& e : o.elements) {
                j.beginObject();
                _snwprintf_s(t, 16, _TRUNCATE, L"%08x", e.id);
                j.kv(L"id", t); j.kv(L"name", bcd::elementName(e.id, o.type)); j.kv(L"registryType", (unsigned long)e.regType);
                j.kv(L"value", elementValueText(e, o.type));
                std::wstring hex;
                for (BYTE b : e.data) { wchar_t h[4]; _snwprintf_s(h, 4, _TRUNCATE, L"%02x", b); hex += h; }
                j.kv(L"raw", hex);
                j.endObject();
            }
            j.endArray();
            j.endObject();
        }
        j.endArray();
        j.endObject();
        logging::outLine(j.str());
        return;
    }
    logging::out(L"store %s: %Iu objects", store.filePath().c_str(), objs.size());
    for (const bcd::Object& o : objs) {
        std::wstring alias = bcd::objectAlias(o.id);
        logging::out(L"");
        logging::out(L"%s  %s%s%s", bcd::objectTypeName(o.type), bcd::guidString(o.id).c_str(), alias.empty() ? L"" : L"  ", alias.c_str());
        logging::out(L"  type 0x%08X, %Iu elements", o.type, o.elements.size());
        for (const bcd::Element& e : o.elements)
            logging::out(L"  %08x  %s", e.id, bcd::describeElement(e, o.type).c_str());
    }
}

int cmdDump(const Args& args)
{
    bcd::Store store;
    bool json = args.has(L"json");
    if (!args.get(L"store").empty() || args.has(L"live")) {
        if (!openStoreFromArgs(args, true, &store)) return isElevated() ? EXIT_FAILED : EXIT_ELEVATION;
        dumpStore(store, json);
        return EXIT_OK;
    }
    if (!requireElevation(L"bcd dump")) return EXIT_ELEVATION;
    LOG_INFO(L"reading disks...");
    std::vector<DiskInfo> disks;
    disk::enumerate(&disks);
    int found = 0;
    for (const Volume& v : allVolumes(disks)) {
        for (const wchar_t* rel : { L"EFI\\Microsoft\\Boot\\BCD", L"Boot\\BCD", L"EFI\\Microsoft\\Recovery\\BCD" }) {
            std::wstring p = v.root + rel;
            if (!exists(p)) continue;
            found++;
            LOG_INFO(L"disk %d partition %d: %s", v.disk->number, v.part->number, p.c_str());
            if (store.openFile(p, true)) { dumpStore(store, json); store.close(); }
            else LOG_WARN(L"if this is the running system's store, use bcd dump --live");
        }
    }
    if (!found) {
        LOG_WARN(L"no BCD file on any attached volume; showing the live system store");
        if (!store.openLiveSystem(true)) return EXIT_FAILED;
        dumpStore(store, json);
    }
    return EXIT_OK;
}

// ---------------------------------------------------------------------------

struct WindowsInstall {
    const Volume* vol;
    std::wstring windir;
    bool hasEfiLoader, hasBiosLoader;
    std::wstring description;
};

std::vector<WindowsInstall> findWindows(const std::vector<Volume>& vols)
{
    std::vector<WindowsInstall> out;
    for (const Volume& v : vols) {
        if (v.part->isEsp() || v.part->isMsr()) continue;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((v.root + L"*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            std::wstring dir = v.root + fd.cFileName;
            if (!exists(dir + L"\\System32\\ntoskrnl.exe")) continue;
            WindowsInstall w;
            w.vol = &v;
            w.windir = fd.cFileName;
            w.hasEfiLoader = exists(dir + L"\\System32\\winload.efi");
            w.hasBiosLoader = exists(dir + L"\\System32\\winload.exe");
            w.description = L"Windows";
            std::wstring krnl = dir + L"\\System32\\ntoskrnl.exe";
            DWORD dummy, sz = GetFileVersionInfoSizeW(krnl.c_str(), &dummy);
            if (sz) {
                std::vector<BYTE> buf(sz);
                if (GetFileVersionInfoW(krnl.c_str(), 0, sz, buf.data())) {
                    VS_FIXEDFILEINFO* ffi; UINT len;
                    if (VerQueryValueW(buf.data(), L"\\", reinterpret_cast<void**>(&ffi), &len)) {
                        wchar_t d[64];
                        DWORD major = HIWORD(ffi->dwProductVersionMS), build = HIWORD(ffi->dwProductVersionLS);
                        const wchar_t* name = major >= 10 ? (build >= 22000 ? L"Windows 11" : L"Windows 10") : major == 6 ? L"Windows Vista/7/8" : L"Windows";
                        _snwprintf_s(d, _countof(d), _TRUNCATE, L"%s (build %lu)", name, build);
                        w.description = d;
                    }
                }
            }
            LOG_INFO(L"found %s at %s%s (loaders: %s%s)", w.description.c_str(), v.root.c_str(), w.windir.c_str(),
                     w.hasEfiLoader ? L"EFI " : L"", w.hasBiosLoader ? L"BIOS" : L"");
            out.push_back(w);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return out;
}

const Volume* pickSystemPartition(const std::vector<Volume>& vols, const WindowsInstall& w, bool uefi)
{
    const Volume* best = nullptr;
    for (const Volume& v : vols) {
        if (uefi) { if (!v.part->isEsp()) continue; }
        else { if (v.disk->gpt || !v.part->mbrActive) continue; }
        if (!best || v.disk == w.vol->disk) best = &v;
    }
    if (!best && !uefi) best = w.vol;  // BIOS: the Windows partition itself can host bootmgr
    return best;
}

bool sameDevice(const bcd::Device& a, const bcd::Device& b)
{
    if (a.kind != b.kind || a.gpt != b.gpt) return false;
    if (a.gpt) return IsEqualGUID(a.gptPartitionId, b.gptPartitionId) && IsEqualGUID(a.gptDiskId, b.gptDiskId);
    return a.mbrPartitionOffset == b.mbrPartitionOffset && a.mbrDiskSignature == b.mbrDiskSignature;
}

bool findExistingEntry(const std::vector<bcd::Object>& objs, const bcd::Device& dev, const std::wstring& sysroot, GUID* out)
{
    for (const bcd::Object& o : objs) {
        if (o.type != bcd::OBJ_OSLOADER) continue;
        bool devMatch = false, rootMatch = sysroot.empty();
        for (const bcd::Element& e : o.elements) {
            if (e.id == bcd::E_OS_OSDEVICE) { bcd::Device d; devMatch = bcd::decodeDevice(e.data.data(), e.data.size(), &d) && sameDevice(d, dev); }
            if (e.id == bcd::E_OS_SYSTEMROOT) { std::wstring s(reinterpret_cast<const wchar_t*>(e.data.data()), e.data.size() / 2); while (!s.empty() && s.back() == 0) s.pop_back(); rootMatch = _wcsicmp(s.c_str(), sysroot.c_str()) == 0; }
        }
        if (devMatch && rootMatch) { *out = o.id; return true; }
    }
    return false;
}

int cmdRebuild(const Args& args)
{
    bool dryRun = args.has(L"dry-run");
    std::wstring fwArg = args.get(L"firmware");
    if (!fwArg.empty() && _wcsicmp(fwArg.c_str(), L"UEFI") != 0 && _wcsicmp(fwArg.c_str(), L"BIOS") != 0) {
        LOG_ERROR(L"--firmware must be UEFI or BIOS, got '%s'", fwArg.c_str());
        return EXIT_USAGE;
    }
    if (!requireElevation(L"bcd rebuild")) return EXIT_ELEVATION;
    LOG_INFO(L"reading disks...");
    std::vector<DiskInfo> disks;
    if (!disk::enumerate(&disks)) { LOG_ERROR(L"no disk could be opened"); return EXIT_FAILED; }
    std::vector<Volume> vols = allVolumes(disks);

    std::vector<WindowsInstall> installs = findWindows(vols);
    if (!args.operands.empty()) {
        std::vector<WindowsInstall> sel;
        for (const std::wstring& wnt : args.operands) {
            bool hit = false;
            for (const WindowsInstall& w : installs) {
                std::wstring full = w.vol->root + w.windir;
                if (_wcsicmp(full.c_str(), wnt.c_str()) == 0) { sel.push_back(w); hit = true; }
            }
            if (!hit) { LOG_ERROR(L"%s is not a Windows installation that scan can see (needs System32\\ntoskrnl.exe)", wnt.c_str()); return EXIT_FAILED; }
        }
        installs = sel;
    }
    if (installs.empty()) {
        LOG_ERROR(L"no Windows installation found on the attached disks; nothing to add");
        return EXIT_FAILED;
    }

    std::wstring fw = args.get(L"firmware");
    bool uefi;
    if (_wcsicmp(fw.c_str(), L"UEFI") == 0) uefi = true;
    else if (_wcsicmp(fw.c_str(), L"BIOS") == 0) uefi = false;
    else if (fw.empty()) {
        uefi = installs[0].vol->disk->gpt;
        LOG_INFO(L"the Windows disk is %s, assuming %s firmware (--firmware overrides)", uefi ? L"GPT" : L"MBR", uefi ? L"UEFI" : L"BIOS");
    } else { LOG_ERROR(L"--firmware must be UEFI or BIOS, got '%s'", fw.c_str()); return EXIT_USAGE; }

    const Volume* sys = nullptr;
    if (!args.get(L"esp").empty()) {
        sys = findVolume(vols, disks, args.get(L"esp"));
        if (!sys) { LOG_ERROR(L"--esp %s does not name a partition that scan can see", args.get(L"esp").c_str()); return EXIT_FAILED; }
    } else {
        sys = pickSystemPartition(vols, installs[0], uefi);
    }
    if (!sys) {
        LOG_ERROR(L"no system partition found (UEFI: an EFI System Partition; BIOS: an active MBR partition); name one with --esp");
        return EXIT_FAILED;
    }
    LOG_INFO(L"system partition: disk %d partition %d (%s, %s) -> %s", sys->disk->number, sys->part->number,
             sys->part->driveLetter.empty() ? L"no letter" : sys->part->driveLetter.c_str(),
             sys->part->fileSystem.c_str(), sys->root.c_str());
    if (uefi && sys->part->fileSystem != L"FAT32" && sys->part->fileSystem != L"FAT")
        LOG_WARN(L"UEFI firmware needs a FAT system partition; this one is %s", sys->part->fileSystem.c_str());

    const WindowsInstall& src = installs[0];
    std::wstring srcBoot = src.vol->root + src.windir + L"\\Boot";
    std::wstring dest = sys->root + (uefi ? L"EFI\\Microsoft\\Boot" : L"Boot");
    std::wstring bcdPath = dest + L"\\BCD";
    std::wstring templ = args.get(L"template");
    if (templ.empty()) templ = src.vol->root + src.windir + L"\\System32\\config\\BCD-Template";
    std::wstring locale = args.get(L"locale", L"en-US");
    bool recreate = args.has(L"recreate") || !exists(bcdPath);

    wchar_t q[600];
    _snwprintf_s(q, _countof(q), _TRUNCATE, L"Copy boot files from %s to %s, %s %s and add up to %Iu Windows entr%s?",
                 srcBoot.c_str(), dest.c_str(), recreate ? L"create" : L"update", bcdPath.c_str(),
                 installs.size(), installs.size() == 1 ? L"y" : L"ies");
    int c = confirmOrDryRun(args, q);
    if (c >= 0 && !dryRun) return c;

    int failures = 0, copied = 0;
    if (uefi) {
        copied += copyTree(srcBoot + L"\\EFI", dest, dryRun, &failures);
        copied += copyTree(srcBoot + L"\\Fonts", dest + L"\\Fonts", dryRun, &failures);
        if (isDir(srcBoot + L"\\Resources")) copied += copyTree(srcBoot + L"\\Resources", dest + L"\\Resources", dryRun, &failures);
        if (makeDir(sys->root + L"EFI\\Boot", dryRun) && copyFileLogged(srcBoot + L"\\EFI\\bootmgfw.efi", sys->root + L"EFI\\Boot\\bootx64.efi", dryRun)) copied++;
        else failures++;
    } else {
        copied += copyTree(srcBoot + L"\\PCAT", dest, dryRun, &failures);
        copied += copyTree(srcBoot + L"\\Fonts", dest + L"\\Fonts", dryRun, &failures);
        if (isDir(srcBoot + L"\\Resources")) copied += copyTree(srcBoot + L"\\Resources", dest + L"\\Resources", dryRun, &failures);
        if (copyFileLogged(srcBoot + L"\\PCAT\\bootmgr", sys->root + L"bootmgr", dryRun)) {
            copied++;
            if (!dryRun) SetFileAttributesW((sys->root + L"bootmgr").c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_READONLY);
        } else failures++;
    }
    LOG_INFO(L"boot files: %d copied, %d not copied", copied, failures);
    if (failures) { LOG_ERROR(L"stopped before the BCD because %d boot files could not be copied (see above)", failures); return EXIT_FAILED; }
    if (dryRun) { LOG_INFO(L"dry run: would now %s %s", recreate ? L"build" : L"update", bcdPath.c_str()); return EXIT_OK; }

    bcd::Store store;
    bool merge = false;
    if (!recreate) {
        if (store.openFile(bcdPath, false)) {
            merge = true;
            LOG_INFO(L"existing store loaded; adding what is missing (--recreate starts from the template)");
        } else {
            LOG_WARN(L"the existing BCD cannot be loaded; it is recreated");
        }
    }
    if (!merge) {
        if (exists(bcdPath)) {
            std::wstring bak = backupDir(args) + L"\\BCD-" + std::to_wstring(GetTickCount64()) + L".bak";
            if (CopyFileW(bcdPath.c_str(), bak.c_str(), FALSE)) LOG_INFO(L"existing BCD saved as %s", bak.c_str());
            else LOG_WARN(L"the existing BCD could not be saved: %s", logging::win32Error(GetLastError()).c_str());
            SetFileAttributesW(bcdPath.c_str(), FILE_ATTRIBUTE_NORMAL);
            DeleteFileW((bcdPath + L".LOG").c_str());
            DeleteFileW((bcdPath + L".LOG1").c_str());
            DeleteFileW((bcdPath + L".LOG2").c_str());
        }
        if (exists(templ)) {
            LOG_INFO(L"using template %s", templ.c_str());
            if (!copyFileLogged(templ, bcdPath, false)) return EXIT_FAILED;
            SetFileAttributesW(bcdPath.c_str(), FILE_ATTRIBUTE_NORMAL);
            if (!store.openFile(bcdPath, false)) return EXIT_FAILED;
            std::vector<bcd::Object> objs;
            store.listObjects(&objs);
            for (const bcd::Object& o : objs)
                if (o.type == bcd::OBJ_OSLOADER || o.type == bcd::OBJ_RESUME) {
                    LOG_DEBUG(L"removing template object %s (%s)", bcd::guidString(o.id).c_str(), bcd::objectTypeName(o.type));
                    store.deleteObject(o.id);
                }
        } else {
            LOG_WARN(L"no BCD-Template at %s; a minimal store is created like bootrec does", templ.c_str());
            if (!store.createFile(bcdPath)) return EXIT_FAILED;
        }
    } else {
        std::wstring bak = backupDir(args) + L"\\BCD-" + std::to_wstring(GetTickCount64()) + L".bak";
        if (store.exportTo(bak)) LOG_INFO(L"existing BCD exported to %s before editing", bak.c_str());
    }
    bool haveSettings = store.objectExists(bcd::GUID_BOOTLOADERSETTINGS);

    bcd::Device sysDev = bcd::partitionDevice(*sys->disk, *sys->part);
    LOG_DEBUG(L"system partition device element: %s", sysDev.describe().c_str());

    bool newBootmgr = !store.objectExists(bcd::GUID_BOOTMGR);
    if (newBootmgr) store.createObject(bcd::GUID_BOOTMGR, bcd::OBJ_BOOTMGR);
    store.setDevice(bcd::GUID_BOOTMGR, bcd::E_DEVICE, sysDev);
    store.setString(bcd::GUID_BOOTMGR, bcd::E_PATH, uefi ? L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi" : L"\\bootmgr");
    if (newBootmgr || !merge) {
        store.setString(bcd::GUID_BOOTMGR, bcd::E_DESCRIPTION, L"Windows Boot Manager");
        store.setString(bcd::GUID_BOOTMGR, bcd::E_LOCALE, locale);
        store.setInteger(bcd::GUID_BOOTMGR, bcd::E_BM_TIMEOUT, 30);
        if (haveSettings) store.setGuidList(bcd::GUID_BOOTMGR, bcd::E_INHERIT, { bcd::GUID_GLOBALSETTINGS });
    }

    bool newMemdiag = !store.objectExists(bcd::GUID_MEMDIAG);
    if (newMemdiag) store.createObject(bcd::GUID_MEMDIAG, bcd::OBJ_MEMDIAG);
    store.setDevice(bcd::GUID_MEMDIAG, bcd::E_DEVICE, sysDev);
    store.setString(bcd::GUID_MEMDIAG, bcd::E_PATH, uefi ? L"\\EFI\\Microsoft\\Boot\\memtest.efi" : L"\\boot\\memtest.exe");
    if (newMemdiag) {
        store.setString(bcd::GUID_MEMDIAG, bcd::E_DESCRIPTION, L"Windows Memory Diagnostic");
        store.setString(bcd::GUID_MEMDIAG, bcd::E_LOCALE, locale);
        if (haveSettings) store.setGuidList(bcd::GUID_MEMDIAG, bcd::E_INHERIT, { bcd::GUID_GLOBALSETTINGS });
        store.setBoolean(bcd::GUID_MEMDIAG, 0x1600000b /*badmemoryaccess*/, true);
        store.setGuidList(bcd::GUID_BOOTMGR, bcd::E_BM_TOOLSDISPLAYORDER, { bcd::GUID_MEMDIAG });
    }

    std::vector<bcd::Object> existing;
    store.listObjects(&existing);
    std::vector<GUID> order;
    store.getGuidList(bcd::GUID_BOOTMGR, bcd::E_BM_DISPLAYORDER, &order);
    {
        std::vector<GUID> keep;
        for (const GUID& g : order) if (store.objectExists(g)) keep.push_back(g); else LOG_INFO(L"displayorder referenced the missing object %s; dropped", bcd::guidString(g).c_str());
        order = keep;
    }
    int added = 0;
    for (const WindowsInstall& w : installs) {
        bcd::Device osDev = bcd::partitionDevice(*w.vol->disk, *w.vol->part);
        std::wstring sysroot = L"\\" + w.windir;
        GUID os;
        if (findExistingEntry(existing, osDev, sysroot, &os)) {
            LOG_INFO(L"%s%s is already in the store as %s; kept", w.vol->root.c_str(), w.windir.c_str(), bcd::guidString(os).c_str());
            bool listed = false;
            for (const GUID& g : order) listed = listed || IsEqualGUID(g, os);
            if (!listed) order.push_back(os);
            continue;
        }
        os = bcd::newGuid();
        GUID resume = bcd::newGuid();
        LOG_INFO(L"adding %s: %s%s as %s", w.description.c_str(), w.vol->root.c_str(), w.windir.c_str(), bcd::guidString(os).c_str());
        store.createObject(os, bcd::OBJ_OSLOADER);
        store.setDevice(os, bcd::E_DEVICE, osDev);
        store.setDevice(os, bcd::E_OS_OSDEVICE, osDev);
        store.setString(os, bcd::E_PATH, sysroot + (uefi ? L"\\system32\\winload.efi" : L"\\system32\\winload.exe"));
        store.setString(os, bcd::E_OS_SYSTEMROOT, sysroot);
        store.setString(os, bcd::E_DESCRIPTION, w.description);
        store.setString(os, bcd::E_LOCALE, locale);
        store.setInteger(os, bcd::E_OS_BOOTMENUPOLICY, 1);
        store.setInteger(os, bcd::E_OS_NX, 0);
        if (haveSettings) store.setGuidList(os, bcd::E_INHERIT, { bcd::GUID_BOOTLOADERSETTINGS });

        store.createObject(resume, bcd::OBJ_RESUME);
        store.setDevice(resume, bcd::E_DEVICE, osDev);
        store.setDevice(resume, bcd::E_RESUME_FILEDEVICE, osDev);
        store.setString(resume, bcd::E_PATH, sysroot + (uefi ? L"\\system32\\winresume.efi" : L"\\system32\\winresume.exe"));
        store.setString(resume, bcd::E_RESUME_FILEPATH, L"\\hiberfil.sys");
        store.setString(resume, bcd::E_DESCRIPTION, L"Windows Resume Application");
        store.setString(resume, bcd::E_LOCALE, locale);
        store.setInteger(resume, bcd::E_RESUME_BOOTMENUPOLICY, 1);
        if (haveSettings) store.setGuidList(resume, bcd::E_INHERIT, { bcd::GUID_RESUMELOADERSETTINGS });
        store.setGuid(os, bcd::E_OS_RESUMEOBJECT, resume);
        order.push_back(os);
        added++;
    }
    if (order.empty()) { LOG_ERROR(L"no boot entry ended up in the display order"); store.close(); return EXIT_FAILED; }
    store.setGuidList(bcd::GUID_BOOTMGR, bcd::E_BM_DISPLAYORDER, order);
    std::vector<GUID> def;
    bool defOk = store.getGuidList(bcd::GUID_BOOTMGR, bcd::E_BM_DEFAULT, &def) && !def.empty() && store.objectExists(def[0]);
    if (!defOk || !merge) store.setGuid(bcd::GUID_BOOTMGR, bcd::E_BM_DEFAULT, order[0]);
    store.markAsSystemStore();
    store.flush();
    store.close();
    SetFileAttributesW(bcdPath.c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
    LOG_INFO(L"BCD %s: %s (%d entries added, %Iu in the display order); next: bootfix bcd dump --store %s",
             merge ? L"updated" : L"written", bcdPath.c_str(), added, order.size(), bcdPath.c_str());
    if (uefi)
        LOG_INFO(L"the firmware NVRAM entry is not touched; \\EFI\\Boot\\bootx64.efi is in place so the firmware boot menu finds the disk, and bcdboot on the target registers it later");
    return EXIT_OK;
}

// ---------------------------------------------------------------------------

bool parseDevice(const std::wstring& v, bcd::Device* out)
{
    if (_wcsicmp(v.c_str(), L"boot") == 0) { *out = bcd::bootDevice(); return true; }
    if (_wcsnicmp(v.c_str(), L"partition=", 10) != 0) {
        LOG_ERROR(L"a device is 'boot' or 'partition=X:' / 'partition=\\Device\\HarddiskVolumeN', got '%s'", v.c_str());
        return false;
    }
    std::vector<DiskInfo> disks;
    disk::enumerate(&disks);
    const DiskInfo* d; const PartitionInfo* p;
    if (!disk::findPartition(disks, v.substr(10), &d, &p)) {
        LOG_ERROR(L"no partition matches '%s' (bootfix scan lists them)", v.substr(10).c_str());
        return false;
    }
    *out = bcd::partitionDevice(*d, *p);
    LOG_DEBUG(L"%s -> disk %d partition %d -> %s", v.c_str(), d->number, p->number, out->describe().c_str());
    return true;
}

bool parseBool(const std::wstring& v, bool* out)
{
    const wchar_t* yes[] = { L"yes", L"true", L"on", L"1" }, * no[] = { L"no", L"false", L"off", L"0" };
    for (auto s : yes) if (_wcsicmp(s, v.c_str()) == 0) { *out = true; return true; }
    for (auto s : no) if (_wcsicmp(s, v.c_str()) == 0) { *out = false; return true; }
    return false;
}

std::vector<std::wstring> splitList(const std::wstring& s)
{
    std::vector<std::wstring> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t c = s.find(L',', start);
        if (c == std::wstring::npos) c = s.size();
        if (c > start) out.push_back(s.substr(start, c - start));
        start = c + 1;
    }
    return out;
}

bool entryFromArgs(const Args& args, bcd::Store& store, GUID* obj, bcd::Object* o)
{
    std::wstring e = args.get(L"entry");
    if (e.empty()) { LOG_ERROR(L"say which object: --entry {bootmgr} or --entry {guid}"); return false; }
    if (!bcd::parseObjectRef(e, obj)) { LOG_ERROR(L"'%s' is not an alias like {bootmgr} or a {guid}", e.c_str()); return false; }
    if (!store.readObject(*obj, o)) { LOG_ERROR(L"object %s is not in the store (bcd dump lists them)", e.c_str()); return false; }
    return true;
}

int cmdSet(const Args& args)
{
    if (args.operands.empty()) { LOG_ERROR(L"bcd set needs NAME=VALUE operands, e.g. timeout=5"); return EXIT_USAGE; }
    bcd::Store store;
    if (!openStoreFromArgs(args, false, &store)) return storeExit(args);
    GUID obj; bcd::Object o;
    if (!entryFromArgs(args, store, &obj, &o)) return EXIT_FAILED;
    bool dry = args.has(L"dry-run");
    for (const std::wstring& op : args.operands) {
        size_t eq = op.find(L'=');
        if (eq == std::wstring::npos) { LOG_ERROR(L"'%s' is not NAME=VALUE", op.c_str()); return EXIT_USAGE; }
        std::wstring name = op.substr(0, eq), val = op.substr(eq + 1);
        DWORD id;
        if (!bcd::elementIdFromName(name, o.type, &id)) { LOG_ERROR(L"'%s' is not an element of %s; use a bcdedit name or custom:XXXXXXXX", name.c_str(), bcd::objectTypeName(o.type)); return EXIT_USAGE; }
        LOG_INFO(L"%s %s (%08x) = %s", bcd::guidString(obj).c_str(), bcd::elementName(id, o.type).c_str(), id, val.c_str());
        if (dry) continue;
        bool ok = false;
        switch ((id >> 24) & 0xF) {
        case 1: { bcd::Device d; ok = parseDevice(val, &d) && store.setDevice(obj, id, d); break; }
        case 2: ok = store.setString(obj, id, val); break;
        case 3: { GUID g; ok = bcd::parseObjectRef(val, &g) && store.setGuid(obj, id, g); if (!ok) LOG_ERROR(L"'%s' is not an object reference", val.c_str()); break; }
        case 4: {
            std::vector<GUID> list;
            for (auto& v : splitList(val)) { GUID g; if (!bcd::parseObjectRef(v, &g)) { LOG_ERROR(L"'%s' is not an object reference", v.c_str()); return EXIT_USAGE; } list.push_back(g); }
            ok = store.setGuidList(obj, id, list);
            break;
        }
        case 5: ok = store.setInteger(obj, id, _wcstoui64(val.c_str(), nullptr, 0)); break;
        case 6: { bool b; if (!parseBool(val, &b)) { LOG_ERROR(L"'%s' is not yes or no", val.c_str()); return EXIT_USAGE; } ok = store.setBoolean(obj, id, b); break; }
        case 7: {
            std::vector<ULONGLONG> list;
            for (auto& v : splitList(val)) list.push_back(_wcstoui64(v.c_str(), nullptr, 0));
            ok = store.setBinary(obj, id, list.data(), (DWORD)(list.size() * 8));
            break;
        }
        default: LOG_ERROR(L"element format %lu cannot be set", (id >> 24) & 0xF); return EXIT_USAGE;
        }
        if (!ok) return EXIT_FAILED;
    }
    if (dry) LOG_INFO(L"dry run: nothing written");
    return EXIT_OK;
}

int cmdUnset(const Args& args)
{
    if (args.operands.empty()) { LOG_ERROR(L"bcd unset needs element names, e.g. bootsequence"); return EXIT_USAGE; }
    bcd::Store store;
    if (!openStoreFromArgs(args, false, &store)) return storeExit(args);
    GUID obj; bcd::Object o;
    if (!entryFromArgs(args, store, &obj, &o)) return EXIT_FAILED;
    for (const std::wstring& name : args.operands) {
        DWORD id;
        if (!bcd::elementIdFromName(name, o.type, &id)) { LOG_ERROR(L"'%s' is not an element of %s", name.c_str(), bcd::objectTypeName(o.type)); return EXIT_USAGE; }
        if (args.has(L"dry-run")) { LOG_INFO(L"dry run: would remove %08x", id); continue; }
        if (!store.deleteElement(obj, id)) return EXIT_FAILED;
        LOG_INFO(L"%s: %s removed", bcd::guidString(obj).c_str(), bcd::elementName(id, o.type).c_str());
    }
    return EXIT_OK;
}

int cmdCreate(const Args& args)
{
    DWORD type;
    if (!bcd::objectTypeFromName(args.get(L"type"), &type)) { LOG_ERROR(L"--type is needed: osloader, resume, bootmgr, memdiag, fwbootmgr, ntldr, bootapp or a hex value"); return EXIT_USAGE; }
    bcd::Store store;
    if (!openStoreFromArgs(args, false, &store)) return storeExit(args);
    GUID id = bcd::newGuid();
    if (!args.get(L"entry").empty() && !bcd::parseObjectRef(args.get(L"entry"), &id)) { LOG_ERROR(L"'%s' is not an alias or a {guid}", args.get(L"entry").c_str()); return EXIT_USAGE; }
    if (store.objectExists(id)) { LOG_ERROR(L"%s already exists", bcd::guidString(id).c_str()); return EXIT_FAILED; }
    if (args.has(L"dry-run")) { LOG_INFO(L"dry run: would create %s type 0x%08X", bcd::guidString(id).c_str(), type); return EXIT_OK; }
    if (!store.createObject(id, type)) return EXIT_FAILED;
    if (!args.get(L"description").empty()) store.setString(id, bcd::E_DESCRIPTION, args.get(L"description"));
    logging::out(L"%s", bcd::guidString(id).c_str());
    LOG_INFO(L"created %s (%s); next: bootfix bcd set --entry %s ...", bcd::guidString(id).c_str(), bcd::objectTypeName(type), bcd::guidString(id).c_str());
    return EXIT_OK;
}

int cmdDelete(const Args& args)
{
    bcd::Store store;
    if (!openStoreFromArgs(args, false, &store)) return storeExit(args);
    GUID obj; bcd::Object o;
    if (!entryFromArgs(args, store, &obj, &o)) return EXIT_FAILED;
    wchar_t q[160];
    _snwprintf_s(q, _countof(q), _TRUNCATE, L"Delete %s %s (%s) from %s?", bcd::objectTypeName(o.type), bcd::guidString(obj).c_str(), bcd::objectAlias(obj).c_str(), store.filePath().c_str());
    int c = confirmOrDryRun(args, q);
    if (c >= 0) return c;
    std::vector<GUID> order;
    if (store.getGuidList(bcd::GUID_BOOTMGR, bcd::E_BM_DISPLAYORDER, &order)) {
        std::vector<GUID> keep;
        for (auto& g : order) if (!IsEqualGUID(g, obj)) keep.push_back(g);
        if (keep.size() != order.size()) store.setGuidList(bcd::GUID_BOOTMGR, bcd::E_BM_DISPLAYORDER, keep);
    }
    if (!store.deleteObject(obj)) return EXIT_FAILED;
    LOG_INFO(L"%s deleted and removed from the display order", bcd::guidString(obj).c_str());
    return EXIT_OK;
}

int cmdExport(const Args& args)
{
    if (args.operands.empty()) { LOG_ERROR(L"bcd export needs the file to write: bootfix bcd export BCD.bak --live"); return EXIT_USAGE; }
    bcd::Store store;
    if (!openStoreFromArgs(args, true, &store)) return storeExit(args);
    if (args.has(L"dry-run")) { LOG_INFO(L"dry run: would export to %s", args.operands[0].c_str()); return EXIT_OK; }
    return store.exportTo(args.operands[0]) ? EXIT_OK : EXIT_FAILED;
}

}  // namespace

int cmdBcd(const Args& args)
{
    const std::wstring& v = args.verb;
    if (v == L"dump") return cmdDump(args);
    if (v == L"rebuild") return cmdRebuild(args);
    if (v == L"set") return cmdSet(args);
    if (v == L"unset") return cmdUnset(args);
    if (v == L"create") return cmdCreate(args);
    if (v == L"delete") return cmdDelete(args);
    if (v == L"export") return cmdExport(args);
    std::vector<std::wstring> verbs = { L"dump", L"rebuild", L"set", L"unset", L"create", L"delete", L"export" };
    std::wstring s = suggest(v, verbs);
    LOG_ERROR(L"unknown bcd verb '%s'%s; run 'bootfix help bcd'", v.c_str(), s.empty() ? L"" : (L" (did you mean '" + s + L"'?)").c_str());
    return EXIT_USAGE;
}
