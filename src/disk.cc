#include "disk.hh"
#include "log.hh"
#include "ntapi.hh"
#include <winioctl.h>
#include <initguid.h>
#include <diskguid.h>
#include <algorithm>

// Known GPT partition types (diskguid.h has the Microsoft ones; add Linux/Apple basics for reporting).
DEFINE_GUID(GUID_PART_LINUX_DATA, 0x0FC63DAF, 0x8483, 0x4772, 0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4);
DEFINE_GUID(GUID_PART_LINUX_SWAP, 0x0657FD6D, 0xA4AB, 0x43C4, 0x84, 0xE5, 0x09, 0x33, 0xC8, 0x4B, 0x4F, 0x4F);
DEFINE_GUID(GUID_PART_BIOS_BOOT,  0x21686148, 0x6449, 0x6E6F, 0x74, 0x4E, 0x65, 0x65, 0x64, 0x45, 0x46, 0x49);

bool PartitionInfo::isEsp() const      { return gpt ? IsEqualGUID(gptType, PARTITION_SYSTEM_GUID) : mbrType == 0xEF; }
bool PartitionInfo::isMsr() const      { return gpt && IsEqualGUID(gptType, PARTITION_MSFT_RESERVED_GUID); }
bool PartitionInfo::isRecovery() const { return gpt && IsEqualGUID(gptType, PARTITION_MSFT_RECOVERY_GUID); }
bool PartitionInfo::isData() const
{
    if (gpt) return IsEqualGUID(gptType, PARTITION_BASIC_DATA_GUID);
    return mbrType == 0x07 || mbrType == 0x0B || mbrType == 0x0C || mbrType == 0x0E || mbrType == 0x06 || mbrType == 0x01 || mbrType == 0x04 || mbrType == 0x17 || mbrType == 0x27;
}

namespace disk {

std::wstring guidToString(const GUID& g)
{
    wchar_t buf[64];
    _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
                 g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
                 g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return buf;
}

const wchar_t* gptTypeName(const GUID& g)
{
    if (IsEqualGUID(g, PARTITION_SYSTEM_GUID))        return L"EFI System";
    if (IsEqualGUID(g, PARTITION_MSFT_RESERVED_GUID)) return L"MS Reserved";
    if (IsEqualGUID(g, PARTITION_BASIC_DATA_GUID))    return L"Basic Data";
    if (IsEqualGUID(g, PARTITION_MSFT_RECOVERY_GUID)) return L"Recovery";
    if (IsEqualGUID(g, PARTITION_LDM_METADATA_GUID))  return L"LDM Metadata";
    if (IsEqualGUID(g, PARTITION_LDM_DATA_GUID))      return L"LDM Data";
    if (IsEqualGUID(g, GUID_PART_LINUX_DATA))         return L"Linux Data";
    if (IsEqualGUID(g, GUID_PART_LINUX_SWAP))         return L"Linux Swap";
    if (IsEqualGUID(g, GUID_PART_BIOS_BOOT))          return L"BIOS Boot";
    if (IsEqualGUID(g, PARTITION_ENTRY_UNUSED_GUID))  return L"Unused";
    return L"Unknown";
}

const wchar_t* mbrTypeName(BYTE t)
{
    switch (t) {
    case 0x00: return L"Empty";
    case 0x01: return L"FAT12";
    case 0x04: case 0x06: return L"FAT16";
    case 0x05: case 0x0F: return L"Extended";
    case 0x07: return L"NTFS/exFAT/HPFS";
    case 0x0B: case 0x0C: return L"FAT32";
    case 0x0E: return L"FAT16 LBA";
    case 0x17: return L"Hidden NTFS";
    case 0x27: return L"WinRE/OEM";
    case 0x42: return L"Dynamic (LDM)";
    case 0x82: return L"Linux swap";
    case 0x83: return L"Linux";
    case 0xEE: return L"GPT protective";
    case 0xEF: return L"EFI System";
    default:   return L"?";
    }
}

bool findPartition(const std::vector<DiskInfo>& disks, const std::wstring& specIn, const DiskInfo** d, const PartitionInfo** p)
{
    std::wstring spec = specIn;
    if (spec.size() >= 2 && spec[1] == L':') spec = spec.substr(0, 2);
    for (const DiskInfo& dk : disks)
        for (const PartitionInfo& pt : dk.partitions) {
            if ((!pt.driveLetter.empty() && _wcsicmp(pt.driveLetter.c_str(), spec.c_str()) == 0) ||
                (!pt.ntDevice.empty() && _wcsicmp(pt.ntDevice.c_str(), spec.c_str()) == 0) ||
                (!pt.volumeGuidPath.empty() && _wcsicmp(pt.volumeGuidPath.c_str(), spec.c_str()) == 0)) {
                *d = &dk;
                *p = &pt;
                return true;
            }
        }
    return false;
}

HANDLE openPhysical(int diskNumber, bool write)
{
    wchar_t path[64];
    _snwprintf_s(path, _countof(path), _TRUNCATE, L"\\\\.\\PhysicalDrive%d", diskNumber);
    DWORD access = GENERIC_READ | (write ? GENERIC_WRITE : 0);
    HANDLE h = CreateFileW(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        LOG_ERROR(L"CreateFile(%s, %s) failed: %lu (%s)", path, write ? L"rw" : L"ro", e, logging::win32Error(e).c_str());
    } else {
        LOG_TRACE(L"opened %s (%s)", path, write ? L"rw" : L"ro");
    }
    return h;
}

HANDLE openVolume(const std::wstring& pathOrLetter, bool write)
{
    std::wstring path;
    if (pathOrLetter.size() <= 3 && pathOrLetter.size() >= 1 && iswalpha(pathOrLetter[0]))
        path = L"\\\\.\\" + pathOrLetter.substr(0, 1) + L":";
    else if (pathOrLetter.rfind(L"\\Device\\", 0) == 0)
        path = nt::globalRoot(pathOrLetter);
    else
        path = pathOrLetter;
    // Volume GUID paths must not have the trailing backslash for raw access.
    if (path.size() > 4 && path.back() == L'\\')
        path.pop_back();
    DWORD access = GENERIC_READ | (write ? GENERIC_WRITE : 0);
    HANDLE h = CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        LOG_ERROR(L"CreateFile(%s, %s) failed: %lu (%s)", path.c_str(), write ? L"rw" : L"ro", e, logging::win32Error(e).c_str());
    } else {
        LOG_TRACE(L"opened %s (%s)", path.c_str(), write ? L"rw" : L"ro");
    }
    return h;
}

bool readAt(HANDLE h, ULONGLONG offset, void* buf, DWORD len)
{
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(h, li, NULL, FILE_BEGIN)) { LOG_LASTERR(L"SetFilePointerEx"); return false; }
    DWORD got = 0;
    if (!ReadFile(h, buf, len, &got, NULL)) { LOG_LASTERR(L"ReadFile"); return false; }
    if (got != len) { LOG_ERROR(L"short read at 0x%I64X: wanted %lu got %lu", offset, len, got); return false; }
    LOG_TRACE(L"read %lu bytes @ 0x%I64X", len, offset);
    return true;
}

bool writeAt(HANDLE h, ULONGLONG offset, const void* buf, DWORD len)
{
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(h, li, NULL, FILE_BEGIN)) { LOG_LASTERR(L"SetFilePointerEx"); return false; }
    DWORD put = 0;
    if (!WriteFile(h, buf, len, &put, NULL)) { LOG_LASTERR(L"WriteFile"); return false; }
    if (put != len) { LOG_ERROR(L"short write at 0x%I64X: wanted %lu wrote %lu", offset, len, put); return false; }
    FlushFileBuffers(h);
    LOG_DEBUG(L"wrote %lu bytes @ 0x%I64X", len, offset);
    return true;
}

bool lockVolume(HANDLE h, bool dismount)
{
    DWORD ret;
    // The originals retry the lock a few times; a backgrounded indexer often holds it briefly.
    for (int attempt = 1; attempt <= 5; attempt++) {
        if (DeviceIoControl(h, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &ret, NULL)) {
            LOG_DEBUG(L"volume locked (attempt %d)", attempt);
            if (dismount && !DeviceIoControl(h, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &ret, NULL))
                LOG_WARN(L"FSCTL_DISMOUNT_VOLUME failed: %lu", GetLastError());
            return true;
        }
        LOG_WARN(L"FSCTL_LOCK_VOLUME attempt %d failed: %lu (%s)", attempt, GetLastError(), logging::win32Error(GetLastError()).c_str());
        Sleep(500);
    }
    return false;
}

void unlockVolume(HANDLE h)
{
    DWORD ret;
    DeviceIoControl(h, FSCTL_UNLOCK_VOLUME, NULL, 0, NULL, 0, &ret, NULL);
}

static std::wstring busTypeName(STORAGE_BUS_TYPE t)
{
    switch (t) {
    case BusTypeScsi: return L"SCSI"; case BusTypeAtapi: return L"ATAPI"; case BusTypeAta: return L"ATA";
    case BusType1394: return L"1394"; case BusTypeSsa: return L"SSA"; case BusTypeFibre: return L"Fibre";
    case BusTypeUsb: return L"USB"; case BusTypeRAID: return L"RAID"; case BusTypeiScsi: return L"iSCSI";
    case BusTypeSas: return L"SAS"; case BusTypeSata: return L"SATA"; case BusTypeSd: return L"SD";
    case BusTypeMmc: return L"MMC"; case BusTypeVirtual: return L"Virtual"; case BusTypeFileBackedVirtual: return L"VHD";
    case BusTypeSpaces: return L"Spaces"; case BusTypeNvme: return L"NVMe"; case BusTypeSCM: return L"SCM";
    case BusTypeUfs: return L"UFS"; default: return L"Unknown";
    }
}

static bool readDisk(int n, DiskInfo* d)
{
    HANDLE h = openPhysical(n, false);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    d->number = n;
    DWORD ret;

    DISK_GEOMETRY_EX geo = {};
    if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, NULL, 0, &geo, sizeof geo, &ret, NULL)) {
        d->sizeBytes = (ULONGLONG)geo.DiskSize.QuadPart;
        d->bytesPerSector = geo.Geometry.BytesPerSector ? geo.Geometry.BytesPerSector : 512;
    } else {
        LOG_WARN(L"disk %d: IOCTL_DISK_GET_DRIVE_GEOMETRY_EX failed: %lu", n, GetLastError());
    }

    STORAGE_PROPERTY_QUERY q = {};
    q.PropertyId = StorageDeviceProperty;
    q.QueryType = PropertyStandardQuery;
    std::vector<BYTE> buf(4096);
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, buf.data(), (DWORD)buf.size(), &ret, NULL)) {
        auto* desc = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(buf.data());
        d->busType = busTypeName(desc->BusType);
        if (desc->ProductIdOffset) {
            std::string s(reinterpret_cast<char*>(buf.data()) + desc->ProductIdOffset);
            while (!s.empty() && s.back() == ' ') s.pop_back();
            d->model.assign(s.begin(), s.end());
        }
    }

    if (!readAt(h, 0, d->sector0, d->bytesPerSector > sizeof d->sector0 ? (DWORD)sizeof d->sector0 : d->bytesPerSector))
        LOG_WARN(L"disk %d: cannot read sector 0", n);

    std::vector<BYTE> layout(sizeof(DRIVE_LAYOUT_INFORMATION_EX) + 128 * sizeof(PARTITION_INFORMATION_EX));
    if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, NULL, 0, layout.data(), (DWORD)layout.size(), &ret, NULL)) {
        auto* L = reinterpret_cast<DRIVE_LAYOUT_INFORMATION_EX*>(layout.data());
        d->layoutRead = true;
        d->gpt = L->PartitionStyle == PARTITION_STYLE_GPT;
        if (d->gpt) d->gptDiskId = L->Gpt.DiskId; else d->mbrSignature = L->Mbr.Signature;
        for (DWORD i = 0; i < L->PartitionCount; i++) {
            const PARTITION_INFORMATION_EX& p = L->PartitionEntry[i];
            if (p.PartitionNumber == 0 || p.PartitionLength.QuadPart == 0)
                continue;  // empty MBR slot
            PartitionInfo pi;
            pi.number = (int)p.PartitionNumber;
            pi.startLba = (ULONGLONG)p.StartingOffset.QuadPart / d->bytesPerSector;
            pi.sizeBytes = (ULONGLONG)p.PartitionLength.QuadPart;
            pi.gpt = d->gpt;
            if (d->gpt) {
                pi.gptType = p.Gpt.PartitionType;
                pi.gptId = p.Gpt.PartitionId;
                pi.gptName = p.Gpt.Name;
            } else {
                pi.mbrType = p.Mbr.PartitionType;
                pi.mbrActive = p.Mbr.BootIndicator != 0;
            }
            d->partitions.push_back(pi);
        }
        std::sort(d->partitions.begin(), d->partitions.end(),
                  [](const PartitionInfo& a, const PartitionInfo& b) { return a.startLba < b.startLba; });
    } else {
        LOG_WARN(L"disk %d: IOCTL_DISK_GET_DRIVE_LAYOUT_EX failed: %lu (%s)", n, GetLastError(), logging::win32Error(GetLastError()).c_str());
    }
    CloseHandle(h);
    LOG_DEBUG(L"disk %d: %s %s, %I64u MB, %s, %Iu partitions", n, d->busType.c_str(), d->model.c_str(),
              d->sizeBytes >> 20, d->gpt ? L"GPT" : L"MBR", d->partitions.size());
    return true;
}

static void mapVolumes(std::vector<DiskInfo>* disks)
{
    wchar_t vol[MAX_PATH];
    HANDLE f = FindFirstVolumeW(vol, _countof(vol));
    if (f == INVALID_HANDLE_VALUE) { LOG_LASTERR(L"FindFirstVolumeW"); return; }
    do {
        std::wstring guidPath = vol;
        std::wstring noSlash = guidPath.substr(0, guidPath.size() - 1);
        wchar_t dev[MAX_PATH] = {};
        QueryDosDeviceW(noSlash.substr(4).c_str(), dev, _countof(dev));  // strip \\?\ -> "Volume{...}"
        HANDLE h = CreateFileW(noSlash.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            LOG_TRACE(L"%s: open failed %lu", vol, GetLastError());
            continue;
        }
        BYTE buf[sizeof(VOLUME_DISK_EXTENTS) + 8 * sizeof(DISK_EXTENT)];
        DWORD ret;
        if (!DeviceIoControl(h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0, buf, sizeof buf, &ret, NULL)) {
            LOG_TRACE(L"%s: no disk extents (%lu)", vol, GetLastError());
            CloseHandle(h);
            continue;
        }
        CloseHandle(h);
        auto* ext = reinterpret_cast<VOLUME_DISK_EXTENTS*>(buf);
        if (ext->NumberOfDiskExtents != 1) {
            LOG_DEBUG(L"%s spans %lu extents (dynamic/spanned) - skipped", vol, ext->NumberOfDiskExtents);
            continue;
        }
        wchar_t letters[MAX_PATH] = {};
        DWORD lettersLen = 0;
        GetVolumePathNamesForVolumeNameW(vol, letters, _countof(letters), &lettersLen);
        std::wstring letter;
        for (wchar_t* p = letters; *p; p += wcslen(p) + 1)
            if (wcslen(p) == 3 && p[1] == L':') { letter.assign(p, 2); break; }

        wchar_t fs[32] = {};
        std::wstring fsName;
        if (GetVolumeInformationW(vol, NULL, 0, NULL, NULL, NULL, fs, _countof(fs)))
            fsName = fs;
        else
            LOG_DEBUG(L"%s: GetVolumeInformation failed %lu (RAW or unmounted)", vol, GetLastError());

        for (DiskInfo& d : *disks) {
            if (d.number != (int)ext->Extents[0].DiskNumber) continue;
            for (PartitionInfo& p : d.partitions) {
                if (p.startLba * d.bytesPerSector != (ULONGLONG)ext->Extents[0].StartingOffset.QuadPart) continue;
                p.ntDevice = dev;
                p.driveLetter = letter;
                p.fileSystem = fsName;
                p.volumeGuidPath = guidPath;
                LOG_TRACE(L"disk %d part %d <- %s %s %s", d.number, p.number, dev, letter.c_str(), fsName.c_str());
            }
        }
    } while (FindNextVolumeW(f, vol, _countof(vol)));
    FindVolumeClose(f);
}

bool enumerate(std::vector<DiskInfo>* out)
{
    // Probe PhysicalDrive0..63; gaps are normal (removed USB sticks keep their numbers).
    int misses = 0;
    for (int n = 0; n < 64 && misses < 8; n++) {
        wchar_t path[64];
        _snwprintf_s(path, _countof(path), _TRUNCATE, L"\\\\.\\PhysicalDrive%d", n);
        HANDLE h = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) { misses++; continue; }
        CloseHandle(h);
        misses = 0;
        DiskInfo d;
        if (readDisk(n, &d))
            out->push_back(d);
    }
    mapVolumes(out);
    LOG_INFO(L"found %Iu physical disk(s)", out->size());
    return !out->empty();
}

}  // namespace disk
