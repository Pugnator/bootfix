// `scan`: read-only diagnosis.  Mirrors what bootrec /ScanOs + bootsect's
// logging + CMbrDisk::DiagnoseBootSectorCode look at.  The report is the
// product and goes to stdout, one record per line, or as JSON (UI-CLI-008).
#include "bootsector.hh"
#include "commands.hh"
#include "disk.hh"
#include "json.hh"
#include "log.hh"
#include "ntapi.hh"
#include <stdio.h>
#include <vector>

namespace {

struct PartReport {
    const PartitionInfo* p;
    bool haveSector = false;
    BootSectorInfo sector;
    std::wstring backupState;                       // "", "valid, identical", ...
    std::vector<std::wstring> findings;             // things that are wrong or notable
    std::vector<std::pair<std::wstring, std::wstring>> files;  // path, meaning
};

struct DiskReport {
    const DiskInfo* d;
    MbrInfo mbr;
    std::vector<std::wstring> findings;
    std::vector<PartReport> parts;
};

bool fileExists(const std::wstring& path)
{
    DWORD a = GetFileAttributesW(path.c_str());
    bool ok = a != INVALID_FILE_ATTRIBUTES;
    LOG_TRACE(L"exists? %s -> %s", path.c_str(), ok ? L"yes" : L"no");
    return ok;
}

std::wstring rootPath(const PartitionInfo& p)
{
    if (!p.driveLetter.empty()) return p.driveLetter + L"\\";
    if (!p.ntDevice.empty()) return nt::globalRoot(p.ntDevice) + L"\\";
    if (!p.volumeGuidPath.empty()) return p.volumeGuidPath;
    return L"";
}

std::wstring firmwareType()
{
    typedef BOOL(WINAPI * Fn)(PFIRMWARE_TYPE);
    Fn fn = (Fn)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetFirmwareType");
    FIRMWARE_TYPE t = FirmwareTypeUnknown;
    if (fn && fn(&t))
        return t == FirmwareTypeUefi ? L"UEFI" : t == FirmwareTypeBios ? L"BIOS" : L"unknown";
    return L"unknown";
}

void scanBootSector(const DiskInfo& d, PartReport& r)
{
    const PartitionInfo& p = *r.p;
    HANDLE h = INVALID_HANDLE_VALUE;
    if (!p.ntDevice.empty()) h = disk::openVolume(p.ntDevice, false);
    bool viaDisk = false;
    if (h == INVALID_HANDLE_VALUE) {
        h = disk::openPhysical(d.number, false);   // RAW / unmounted: read through the disk
        viaDisk = true;
        if (h == INVALID_HANDLE_VALUE) { r.findings.push_back(L"boot sector could not be read"); return; }
    }
    std::vector<BYTE> sec(d.bytesPerSector * 24);
    ULONGLONG base = viaDisk ? p.startLba * d.bytesPerSector : 0;
    if (!disk::readAt(h, base, sec.data(), (DWORD)sec.size())) { CloseHandle(h); r.findings.push_back(L"boot sector could not be read"); return; }
    r.haveSector = true;
    r.sector = bootsector::inspect(sec.data(), d.bytesPerSector);
    const BootSectorInfo& bi = r.sector;
    if (bi.fs == FS_NTFS) {
        std::vector<BYTE> last(d.bytesPerSector);
        ULONGLONG lastOff = viaDisk ? base + p.sizeBytes - d.bytesPerSector : p.sizeBytes - d.bytesPerSector;
        if (disk::readAt(h, lastOff, last.data(), d.bytesPerSector)) {
            BootSectorInfo bb = bootsector::inspect(last.data(), d.bytesPerSector);
            bool same = memcmp(last.data(), sec.data(), d.bytesPerSector) == 0;
            r.backupState = std::wstring(bb.fs == FS_NTFS ? L"last sector holds a valid NTFS boot sector" : L"last sector is not a valid NTFS boot sector") + (same ? L", identical to sector 0" : L", differs from sector 0");
        }
        if (sec.size() >= 0x20C && memcmp(sec.data() + 0x202, L"NTLDR", 10) == 0)
            r.findings.push_back(L"boot area carries NTLDR (XP) boot code; a Vista or later install cannot boot from it");
        if (bi.bytesPerSector && bi.bytesPerSector != d.bytesPerSector) {
            wchar_t b[128];
            _snwprintf_s(b, _countof(b), _TRUNCATE, L"BPB says %lu bytes per sector but the disk reports %lu", bi.bytesPerSector, d.bytesPerSector);
            r.findings.push_back(b);
        }
    } else if (bi.fs == FS_FAT32) {
        BootSectorInfo bb = bootsector::inspect(sec.data() + 6 * (size_t)d.bytesPerSector, d.bytesPerSector);
        const BYTE* s12 = sec.data() + 12 * (size_t)d.bytesPerSector;
        bool s12ok = s12[0x1FE] == 0x55 && s12[0x1FF] == 0xAA;
        r.backupState = std::wstring(bb.fs == FS_FAT32 ? L"sector 6 holds a valid FAT32 boot sector" : L"sector 6 is not a valid FAT32 boot sector") + (s12ok ? L"; sector 12 second stage present" : L"; sector 12 second stage missing");
    }
    if (bi.fs != FS_UNKNOWN && bi.code == CODE_OTHER)
        r.findings.push_back(L"boot code is not the Windows 10 (17763) code; another build or a third-party loader");
    if (bi.fs != FS_UNKNOWN && bi.code == CODE_NONE)
        r.findings.push_back(L"boot code area is empty; BIOS cannot boot this volume (fixboot writes it)");
    if (!p.fileSystem.empty() && bi.fs != FS_UNKNOWN) {
        std::wstring want = bootsector::fsName(bi.fs);
        if (p.fileSystem != want && !(p.fileSystem == L"FAT" && bi.fs == FS_FAT))
            r.findings.push_back(L"mounted as " + p.fileSystem + L" but the boot sector says " + want);
    }
    if (p.fileSystem.empty() && bi.fs == FS_UNKNOWN && !p.isMsr())
        r.findings.push_back(L"not mounted and the boot sector is not recognised (RAW?)");
    logging::hexdump(LOG_LEVEL_TRACE, __func__, L"sector 0", sec.data(), d.bytesPerSector);
    CloseHandle(h);
}

void scanBootFiles(PartReport& r, int* winCount)
{
    std::wstring root = rootPath(*r.p);
    if (root.empty()) return;
    struct { const wchar_t* rel; const wchar_t* what; } files[] = {
        { L"Windows\\System32\\ntoskrnl.exe", L"Windows installation" },
        { L"Windows\\System32\\winload.exe",  L"BIOS OS loader" },
        { L"Windows\\System32\\winload.efi",  L"UEFI OS loader" },
        { L"Windows\\System32\\config\\BCD-Template", L"BCD template (bcdboot source)" },
        { L"Windows\\Boot\\EFI\\bootmgfw.efi", L"UEFI boot manager source" },
        { L"Windows\\Boot\\PCAT\\bootmgr",    L"BIOS boot manager source" },
        { L"bootmgr",                         L"BIOS boot manager" },
        { L"Boot\\BCD",                       L"BIOS BCD store" },
        { L"EFI\\Microsoft\\Boot\\bootmgfw.efi", L"UEFI boot manager" },
        { L"EFI\\Microsoft\\Boot\\BCD",       L"UEFI BCD store" },
        { L"EFI\\Boot\\bootx64.efi",          L"UEFI fallback loader" },
        { L"ntldr",                           L"NTLDR (XP)" },
        { L"boot.ini",                        L"boot.ini (XP)" },
        { L"Recovery\\WindowsRE\\winre.wim",  L"WinRE image" },
    };
    for (auto& f : files) {
        if (fileExists(root + f.rel)) {
            r.files.push_back(std::make_pair(root + f.rel, std::wstring(f.what)));
            if (wcscmp(f.rel, L"Windows\\System32\\ntoskrnl.exe") == 0) ++*winCount;
        }
    }
}

void printHuman(const std::vector<DiskReport>& disks, const std::wstring& fw, int winCount)
{
    logging::out(L"firmware of this machine: %s (the disk may belong to another machine)", fw.c_str());
    for (const DiskReport& dr : disks) {
        const DiskInfo& d = *dr.d;
        logging::out(L"disk %d: %s %s, %I64u MB, %lu B/sector, %s%s", d.number, d.busType.c_str(), d.model.c_str(),
                     d.sizeBytes >> 20, d.bytesPerSector, d.gpt ? L"GPT " : L"MBR",
                     d.gpt ? disk::guidToString(d.gptDiskId).c_str() : L"");
        logging::out(L"disk %d mbr: %s", d.number, dr.mbr.describe().c_str());
        for (const std::wstring& f : dr.findings) logging::out(L"disk %d finding: %s", d.number, f.c_str());
        for (const PartReport& pr : dr.parts) {
            const PartitionInfo& p = *pr.p;
            std::wstring type = p.gpt ? disk::gptTypeName(p.gptType) : disk::mbrTypeName(p.mbrType);
            logging::out(L"disk %d partition %d: %s, %I64u MB, LBA %I64u, %s, %s, %s%s", d.number, p.number, type.c_str(),
                         p.sizeBytes >> 20, p.startLba, p.driveLetter.empty() ? L"no letter" : p.driveLetter.c_str(),
                         p.fileSystem.empty() ? L"no file system" : p.fileSystem.c_str(),
                         p.ntDevice.empty() ? L"no volume device" : p.ntDevice.c_str(),
                         p.mbrActive ? L", active" : p.isEsp() ? L", ESP" : L"");
            if (pr.haveSector) logging::out(L"disk %d partition %d boot sector: %s", d.number, p.number, pr.sector.describe().c_str());
            if (!pr.backupState.empty()) logging::out(L"disk %d partition %d backup: %s", d.number, p.number, pr.backupState.c_str());
            for (auto& f : pr.files) logging::out(L"disk %d partition %d file: %s (%s)", d.number, p.number, f.first.c_str(), f.second.c_str());
            for (auto& f : pr.findings) logging::out(L"disk %d partition %d finding: %s", d.number, p.number, f.c_str());
        }
    }
    logging::out(L"windows installations: %d", winCount);
}

void printJson(const std::vector<DiskReport>& disks, const std::wstring& fw, int winCount)
{
    JsonWriter j;
    j.beginObject();
    j.kv(L"firmware", fw);
    j.kv(L"windowsInstallations", winCount);
    j.key(L"disks"); j.beginArray();
    for (const DiskReport& dr : disks) {
        const DiskInfo& d = *dr.d;
        j.beginObject();
        j.kv(L"number", d.number); j.kv(L"bus", d.busType); j.kv(L"model", d.model);
        j.kv(L"sizeBytes", (unsigned long long)d.sizeBytes); j.kv(L"bytesPerSector", (unsigned long)d.bytesPerSector);
        j.kv(L"style", d.gpt ? L"GPT" : L"MBR");
        if (d.gpt) j.kv(L"diskId", disk::guidToString(d.gptDiskId)); else { wchar_t b[16]; _snwprintf_s(b, 16, _TRUNCATE, L"0x%08X", d.mbrSignature); j.kv(L"diskSignature", b); }
        j.key(L"mbr"); j.beginObject();
        j.kv(L"validSignature", dr.mbr.validSignature); j.kv(L"code", dr.mbr.codeName); j.kv(L"activePartitions", dr.mbr.activeCount);
        j.kv(L"protectiveGpt", dr.mbr.protectiveGpt); j.endObject();
        j.key(L"findings"); j.beginArray(); for (auto& f : dr.findings) j.value(f); j.endArray();
        j.key(L"partitions"); j.beginArray();
        for (const PartReport& pr : dr.parts) {
            const PartitionInfo& p = *pr.p;
            j.beginObject();
            j.kv(L"number", p.number);
            j.kv(L"type", p.gpt ? disk::gptTypeName(p.gptType) : disk::mbrTypeName(p.mbrType));
            if (p.gpt) { j.kv(L"typeGuid", disk::guidToString(p.gptType)); j.kv(L"id", disk::guidToString(p.gptId)); j.kv(L"name", p.gptName); }
            else { j.kv(L"typeByte", (int)p.mbrType); j.kv(L"active", p.mbrActive); }
            j.kv(L"startLba", (unsigned long long)p.startLba); j.kv(L"sizeBytes", (unsigned long long)p.sizeBytes);
            j.kv(L"driveLetter", p.driveLetter); j.kv(L"fileSystem", p.fileSystem); j.kv(L"device", p.ntDevice);
            j.kv(L"volumeGuidPath", p.volumeGuidPath); j.kv(L"esp", p.isEsp()); j.kv(L"msr", p.isMsr());
            if (pr.haveSector) {
                j.key(L"bootSector"); j.beginObject();
                j.kv(L"fileSystem", bootsector::fsName(pr.sector.fs));
                std::wstring oem(pr.sector.oem.begin(), pr.sector.oem.end()); j.kv(L"oem", oem);
                j.kv(L"bytesPerSector", (unsigned long)pr.sector.bytesPerSector); j.kv(L"totalSectors", (unsigned long long)pr.sector.totalSectors);
                j.kv(L"validSignature", pr.sector.validSignature); j.kv(L"code", bootsector::codeName(pr.sector.code));
                j.kv(L"mentionsBootmgr", pr.sector.mentionsBootmgr); j.kv(L"mentionsNtldr", pr.sector.mentionsNtldr);
                j.kv(L"backup", pr.backupState); j.endObject();
            }
            j.key(L"files"); j.beginArray();
            for (auto& f : pr.files) { j.beginObject(); j.kv(L"path", f.first); j.kv(L"meaning", f.second); j.endObject(); }
            j.endArray();
            j.key(L"findings"); j.beginArray(); for (auto& f : pr.findings) j.value(f); j.endArray();
            j.endObject();
        }
        j.endArray();
        j.endObject();
    }
    j.endArray();
    j.endObject();
    logging::outLine(j.str());
}

}  // namespace

int cmdScan(const Args& args)
{
    long long only = -1;
    std::wstring err;
    if (!args.getInt(L"disk", &only, &err) && !err.empty()) { LOG_ERROR(L"%s", err.c_str()); return EXIT_USAGE; }
    if (!requireElevation(L"scan")) return EXIT_ELEVATION;
    LOG_INFO(L"reading disks...");   // something within 100 ms before the slow part (UI-CLI-016)
    std::vector<DiskInfo> disks;
    if (!disk::enumerate(&disks)) {
        LOG_ERROR(L"no disk could be opened");
        return EXIT_FAILED;
    }

    std::vector<DiskReport> report;
    int winCount = 0;
    for (const DiskInfo& d : disks) {
        if (only >= 0 && d.number != only) continue;
        DiskReport dr;
        dr.d = &d;
        dr.mbr = bootsector::inspectMbr(d.sector0, d.bytesPerSector);
        if (!d.gpt && !dr.mbr.validSignature) dr.findings.push_back(L"MBR signature missing; BIOS will not boot this disk (fixmbr writes it)");
        if (!d.gpt && dr.mbr.activeCount == 0) dr.findings.push_back(L"no active partition in the MBR; BIOS needs one to boot");
        if (!d.gpt && dr.mbr.activeCount > 1) dr.findings.push_back(L"more than one active partition; only one may be active");
        if (!d.gpt && dr.mbr.code == CODE_NONE) dr.findings.push_back(L"no MBR boot code; fixmbr writes the Windows code");
        if (!d.layoutRead) dr.findings.push_back(L"partition layout could not be read");
        for (const PartitionInfo& p : d.partitions) {
            PartReport pr;
            pr.p = &p;
            if (!p.isMsr()) {
                scanBootSector(d, pr);
                scanBootFiles(pr, &winCount);
            }
            dr.parts.push_back(pr);
        }
        report.push_back(dr);
    }
    if (only >= 0 && report.empty()) { LOG_ERROR(L"disk %lld not found; scan without --disk lists the numbers", only); return EXIT_FAILED; }
    if (args.has(L"json")) printJson(report, firmwareType(), winCount);
    else printHuman(report, firmwareType(), winCount);
    return EXIT_OK;
}
