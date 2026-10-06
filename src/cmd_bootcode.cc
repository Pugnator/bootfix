// `fixmbr` and `fixboot`: what bootsect.exe /nt60|/nt52 <vol> [/mbr] and
// bootrec /FixMbr + /FixBoot do, reconstructed from their decompilation:
//   * MBR: copy 0x1B8 bytes of boot code, keep disk signature + partition table
//   * VBR: (bootrec) restore sector 0 from the backup boot sector when needed,
//     then (bootsect) keep the BPB (jump target / 0x54 bytes for NTFS), overlay
//     the blob: NTFS = 16 sectors, FAT32 = sector 0 + sector 12, exFAT = 12+12
//   * lock the volume first (FSCTL_LOCK_VOLUME), dismount anyway on --dismount
#include "bootsector.hh"
#include "commands.hh"
#include "disk.hh"
#include "log.hh"
#include <winioctl.h>
#include <stdio.h>
#include <string>
#include <vector>

namespace {

std::wstring timestamp()
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[32];
    _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

// Save the sectors we are about to overwrite (bootrec's CDiskIO::SaveSectors does the same).
bool backupSectors(const Args& args, const std::wstring& tag, const void* data, size_t len)
{
    if (args.has(L"no-backup")) return true;
    std::wstring path = backupDir(args) + L"\\" + tag + L"-" + timestamp() + L".bin";
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { LOG_ERROR(L"cannot create the backup file %s: %s", path.c_str(), logging::win32Error(GetLastError()).c_str()); return false; }
    DWORD w;
    bool ok = WriteFile(h, data, (DWORD)len, &w, NULL) && w == len;
    CloseHandle(h);
    if (ok) LOG_INFO(L"original sectors saved to %s (%Iu bytes)", path.c_str(), len);
    else LOG_ERROR(L"cannot write the backup file %s: %s", path.c_str(), logging::win32Error(GetLastError()).c_str());
    return ok;
}

std::wstring sanitize(std::wstring s)
{
    for (auto& c : s) if (c == L'\\' || c == L':' || c == L'?' || c == L'{' || c == L'}') c = L'_';
    return s;
}

}  // namespace

int cmdFixMbr(const Args& args)
{
    long long diskNo = -1;
    std::wstring err;
    if (!args.getInt(L"disk", &diskNo, &err)) {
        LOG_ERROR(L"%s", err.empty() ? L"fixmbr needs the disk: --disk N (bootfix scan lists the numbers)" : err.c_str());
        return EXIT_USAGE;
    }
    bool nt52 = args.has(L"nt52");
    if (!requireElevation(L"fixmbr")) return EXIT_ELEVATION;

    LOG_INFO(L"reading disks...");
    std::vector<DiskInfo> disks;
    disk::enumerate(&disks);
    const DiskInfo* d = nullptr;
    for (auto& x : disks) if (x.number == diskNo) d = &x;
    if (!d) { LOG_ERROR(L"disk %lld was not found; bootfix scan lists the disks", diskNo); return EXIT_FAILED; }

    MbrInfo cur = bootsector::inspectMbr(d->sector0, d->bytesPerSector);
    LOG_INFO(L"disk %d (%s %s): current MBR: %s", d->number, d->busType.c_str(), d->model.c_str(), cur.describe().c_str());
    logging::hexdump(LOG_LEVEL_DEBUG, __func__, L"current MBR", d->sector0, 512);
    if (d->gpt)
        LOG_WARN(L"disk %d is GPT: only BIOS firmware uses MBR code on it; UEFI ignores it", d->number);
    if (!cur.validSignature && !cur.allZero)
        LOG_WARN(L"the MBR has no 55AA signature; the partition table bytes are kept as they are (check bootfix scan first)");
    if (cur.code == (nt52 ? CODE_NT52 : CODE_NT60))
        LOG_INFO(L"the MBR already carries this boot code; it is rewritten with the same bytes");

    BYTE out[512];
    bootsector::buildMbr(nt52, d->sector0, out);
    int diff = 0;
    for (int i = 0; i < 512; i++) diff += out[i] != d->sector0[i];
    LOG_DEBUG(L"new MBR differs from the current one in %d of 512 bytes", diff);
    logging::hexdump(LOG_LEVEL_TRACE, __func__, L"new MBR", out, 512);

    wchar_t q[160];
    _snwprintf_s(q, _countof(q), _TRUNCATE, L"Write %s boot code to the MBR of disk %d (%s)?", nt52 ? L"NTLDR (NT52)" : L"BOOTMGR (NT60)", d->number, d->model.c_str());
    int c = confirmOrDryRun(args, q);
    if (c >= 0) return c;

    wchar_t tag[32];
    _snwprintf_s(tag, _countof(tag), _TRUNCATE, L"disk%d-mbr", d->number);
    if (!backupSectors(args, tag, d->sector0, 512)) return EXIT_FAILED;

    HANDLE h = disk::openPhysical(d->number, true);
    if (h == INVALID_HANDLE_VALUE) return EXIT_FAILED;
    DWORD ret;
    bool locked = DeviceIoControl(h, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &ret, NULL) != 0;
    LOG_DEBUG(L"disk lock: %s", locked ? L"acquired" : L"not acquired; MBR writes are allowed anyway");
    bool ok = disk::writeAt(h, 0, out, 512);
    if (ok) {
        BYTE verify[512];
        ok = disk::readAt(h, 0, verify, 512) && memcmp(verify, out, 512) == 0;
        if (ok) LOG_INFO(L"disk %d: MBR boot code written and read back; next: bootfix scan --disk %d", d->number, d->number);
        else LOG_ERROR(L"disk %d: the MBR read back differs from what was written; restore the backup if the disk no longer boots", d->number);
    }
    if (locked) DeviceIoControl(h, FSCTL_UNLOCK_VOLUME, NULL, 0, NULL, 0, &ret, NULL);
    CloseHandle(h);
    return ok ? EXIT_OK : EXIT_FAILED;
}

// ---------------------------------------------------------------------------

namespace {

// One volume.  Returns an exit code; EXIT_OK + *skipped when nothing was recognised under --all.
int fixVolume(const Args& args, const std::wstring& vol, bool nt52, bool dismount, bool fromBackup, bool skipUnknown, bool* skipped)
{
    *skipped = false;
    HANDLE h = disk::openVolume(vol, true);
    if (h == INVALID_HANDLE_VALUE) return EXIT_FAILED;

    // Sector size and partition length from the driver (bootsect: BootsectRetrieveVolumeProperties).
    DISK_GEOMETRY geo = {};
    GET_LENGTH_INFORMATION len = {};
    DWORD ret;
    DWORD bps = 512;
    if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY, NULL, 0, &geo, sizeof geo, &ret, NULL) && geo.BytesPerSector)
        bps = geo.BytesPerSector;
    else
        LOG_DEBUG(L"IOCTL_DISK_GET_DRIVE_GEOMETRY: %s; assuming 512 B/sector", logging::win32Error(GetLastError()).c_str());
    if (!DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0, &len, sizeof len, &ret, NULL))
        LOG_DEBUG(L"IOCTL_DISK_GET_LENGTH_INFO: %s; the NTFS backup sector cannot be located", logging::win32Error(GetLastError()).c_str());

    std::vector<BYTE> cur(bps * 24);
    if (!disk::readAt(h, 0, cur.data(), (DWORD)cur.size())) { CloseHandle(h); return EXIT_FAILED; }
    BootSectorInfo bi = bootsector::inspect(cur.data(), bps);
    LOG_INFO(L"%s: current boot sector: %s", vol.c_str(), bi.describe().c_str());
    logging::hexdump(LOG_LEVEL_DEBUG, __func__, L"current sector 0", cur.data(), bps);

    // bootrec /FixBoot: the backup boot sector (NTFS: last sector of the partition, FAT32: sector 6).
    std::vector<BYTE> backup;
    BootSectorInfo backupInfo;
    if ((bi.fs == FS_NTFS || bi.fs == FS_UNKNOWN) && len.Length.QuadPart > (LONGLONG)bps) {
        std::vector<BYTE> last(bps);
        if (disk::readAt(h, (ULONGLONG)len.Length.QuadPart - bps, last.data(), bps)) {
            BootSectorInfo li = bootsector::inspect(last.data(), bps);
            if (li.fs == FS_NTFS && li.validSignature) { backup = last; backupInfo = li; }
        }
    }
    if (backup.empty() && (bi.fs == FS_FAT32 || bi.fs == FS_UNKNOWN)) {
        BootSectorInfo s6 = bootsector::inspect(cur.data() + 6 * (size_t)bps, bps);
        if (s6.fs == FS_FAT32 && s6.validSignature) { backup.assign(cur.begin() + 6 * (size_t)bps, cur.begin() + 7 * (size_t)bps); backupInfo = s6; }
    }
    if (!backup.empty()) {
        bool same = memcmp(backup.data(), cur.data(), bps) == 0;
        LOG_INFO(L"backup boot sector: %s (%s sector 0)", backupInfo.describe().c_str(), same ? L"identical to" : L"differs from");
        if (bi.fs == FS_UNKNOWN) {
            LOG_WARN(L"sector 0 is not a valid boot sector; it is restored from the backup copy like bootrec /FixBoot does");
            fromBackup = true;
        }
        if (fromBackup && !same) {
            memcpy(cur.data(), backup.data(), bps);
            bi = backupInfo;
            LOG_INFO(L"the backup boot sector is the base for the new sector 0");
        } else if (!same) {
            LOG_WARN(L"sector 0 is kept as it is; --from-backup restores the backup copy first");
        }
    }
    if (bi.fs == FS_UNKNOWN) {
        if (skipUnknown) { LOG_INFO(L"%s: no recognised file system, skipped", vol.c_str()); CloseHandle(h); *skipped = true; return EXIT_OK; }
        LOG_ERROR(L"%s: the boot sector is not FAT, FAT32, NTFS or exFAT and no valid backup copy exists; nothing written", vol.c_str());
        CloseHandle(h);
        return EXIT_FAILED;
    }
    if (bi.bytesPerSector && bi.bytesPerSector != bps) {
        LOG_WARN(L"BPB bytes per sector (%lu) differ from the device (%lu); the BPB value is used like bootsect does", bi.bytesPerSector, bps);
        bps = bi.bytesPerSector;
        if (bps < 512 || bps > 4096) { LOG_ERROR(L"%s: sector size %lu is outside 512..4096; nothing written", vol.c_str(), bps); CloseHandle(h); return EXIT_FAILED; }
    }

    std::string img;
    std::wstring why;
    if (!bootsector::buildVolumeBootCode(bi.fs, nt52, bps, cur.data(), cur.size(), &img, &why)) {
        LOG_ERROR(L"%s: %s; nothing written", vol.c_str(), why.c_str());
        CloseHandle(h);
        return EXIT_FAILED;
    }

    wchar_t q[200];
    _snwprintf_s(q, _countof(q), _TRUNCATE, L"Write %s %s boot code to %s (%Iu bytes)?", nt52 ? L"NTLDR (NT52)" : L"BOOTMGR (NT60)", bootsector::fsName(bi.fs), vol.c_str(), img.size());
    int c = confirmOrDryRun(args, q);
    if (c >= 0) { CloseHandle(h); return c; }

    std::vector<BYTE> orig(cur.size());
    disk::readAt(h, 0, orig.data(), (DWORD)orig.size());
    size_t backupLen = img.size() > orig.size() ? orig.size() : img.size();
    if (bi.fs == FS_FAT32) backupLen = 13 * (size_t)bps;  // sectors 0..12 are touched
    if (!backupSectors(args, sanitize(vol), orig.data(), backupLen)) { CloseHandle(h); return EXIT_FAILED; }

    // bootsect: FSCTL_LOCK_VOLUME, then FSCTL_DISMOUNT_VOLUME; with /force dismount even if the lock failed.
    bool locked = disk::lockVolume(h, false);
    if (!locked) {
        if (!dismount) {
            LOG_ERROR(L"%s: the volume could not be locked because something has files open on it; close them or pass --dismount", vol.c_str());
            CloseHandle(h);
            return EXIT_FAILED;
        }
        LOG_WARN(L"--dismount: the volume is dismounted without a lock; open handles on it become invalid");
    }
    if (!DeviceIoControl(h, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &ret, NULL))
        LOG_WARN(L"FSCTL_DISMOUNT_VOLUME: %s", logging::win32Error(GetLastError()).c_str());

    bool ok;
    if (bi.fs == FS_FAT32) {
        ok = disk::writeAt(h, 0, img.data(), 0x200) && disk::writeAt(h, 0x1800, img.data() + 0x400, 0x200);
        if (ok && memcmp(orig.data() + 6 * (size_t)bps, orig.data(), bps) == 0) {
            if (disk::writeAt(h, 6 * (ULONGLONG)bps, img.data(), 0x200))
                LOG_INFO(L"backup boot sector (sector 6) updated as well");
        }
    } else {
        ok = disk::writeAt(h, 0, img.data(), (DWORD)img.size());
    }
    if (ok) {
        std::vector<BYTE> verify(img.size());
        ok = disk::readAt(h, 0, verify.data(), (DWORD)verify.size()) &&
             (bi.fs == FS_FAT32 ? memcmp(verify.data(), img.data(), 0x200) == 0 : memcmp(verify.data(), img.data(), img.size()) == 0);
        if (ok) LOG_INFO(L"%s: %s boot code written and read back", vol.c_str(), bootsector::fsName(bi.fs));
        else LOG_ERROR(L"%s: the boot sector read back differs from what was written; restore the backup if the volume no longer boots", vol.c_str());
    }
    if (locked) disk::unlockVolume(h);
    CloseHandle(h);
    return ok ? EXIT_OK : EXIT_FAILED;
}

}  // namespace

int cmdFixBoot(const Args& args)
{
    bool nt52 = args.has(L"nt52");
    bool dismount = args.has(L"dismount");
    bool fromBackup = args.has(L"from-backup");
    bool skipped;
    if (!args.has(L"all") && args.operands.empty() && args.get(L"volume").empty()) {
        LOG_ERROR(L"fixboot needs a volume: bootfix fixboot F: (or \\Device\\HarddiskVolumeN), or --all");
        return EXIT_USAGE;
    }
    if (!requireElevation(L"fixboot")) return EXIT_ELEVATION;

    if (args.has(L"all")) {
        LOG_INFO(L"reading disks...");
        std::vector<DiskInfo> disks;
        if (!disk::enumerate(&disks)) return EXIT_FAILED;
        int failed = 0, done = 0;
        for (const DiskInfo& d : disks)
            for (const PartitionInfo& p : d.partitions) {
                if (p.ntDevice.empty() || p.isMsr()) continue;
                int rc = fixVolume(args, p.ntDevice, nt52, dismount, fromBackup, true, &skipped);
                if (rc == EXIT_OK && !skipped) done++; else if (rc != EXIT_OK) failed++;
            }
        LOG_INFO(L"fixboot --all: %d volumes updated, %d failed", done, failed);
        return failed ? EXIT_FAILED : EXIT_OK;
    }

    std::vector<std::wstring> vols = args.operands;
    if (!args.get(L"volume").empty()) vols.push_back(args.get(L"volume"));
    if (vols.empty()) {
        LOG_ERROR(L"fixboot needs a volume: bootfix fixboot F: (or \\Device\\HarddiskVolumeN), or --all");
        return EXIT_USAGE;
    }
    int worst = EXIT_OK;
    for (const std::wstring& v : vols) {
        int rc = fixVolume(args, v, nt52, dismount, fromBackup, false, &skipped);
        if (rc > worst) worst = rc;
    }
    if (worst == EXIT_OK) LOG_INFO(L"next: bootfix scan");
    return worst;
}
