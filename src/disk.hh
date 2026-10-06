// Disk / partition / volume model used by every command.
#pragma once
#include <windows.h>
#include <string>
#include <vector>

struct PartitionInfo {
    int   number = 0;            // 1-based, as in \Device\HarddiskN\PartitionM
    ULONGLONG startLba = 0;
    ULONGLONG sizeBytes = 0;
    bool  gpt = false;
    BYTE  mbrType = 0;           // MBR: partition type byte
    bool  mbrActive = false;     // MBR: boot indicator 0x80
    GUID  gptType = {};          // GPT: partition type GUID
    GUID  gptId = {};            // GPT: unique partition GUID
    std::wstring gptName;
    std::wstring ntDevice;       // \Device\HarddiskVolumeN (if a volume is mounted on it)
    std::wstring driveLetter;    // "C:" or ""
    std::wstring fileSystem;     // from GetVolumeInformation, or "" when unreadable
    std::wstring volumeGuidPath; // \\?\Volume{...}\ or ""
    bool  isEsp() const;         // EFI System Partition (GPT type or MBR 0xEF)
    bool  isMsr() const;
    bool  isRecovery() const;
    bool  isData() const;
};

struct DiskInfo {
    int   number = 0;            // \\.\PhysicalDriveN
    ULONGLONG sizeBytes = 0;
    DWORD bytesPerSector = 512;
    bool  gpt = false;
    bool  layoutRead = false;    // false when IOCTL_DISK_GET_DRIVE_LAYOUT_EX failed
    DWORD mbrSignature = 0;
    GUID  gptDiskId = {};
    std::wstring busType;
    std::wstring model;
    BYTE  sector0[4096] = {};    // first sector (MBR / protective MBR)
    std::vector<PartitionInfo> partitions;
};

namespace disk {

// Enumerate all physical disks and their partitions, and map mounted
// volumes (drive letters, file systems) onto partitions.
bool enumerate(std::vector<DiskInfo>* out);

// Resolve "X:", "\Device\HarddiskVolumeN" or "\\?\Volume{..}\" to its disk/partition.
bool findPartition(const std::vector<DiskInfo>& disks, const std::wstring& spec, const DiskInfo** d, const PartitionInfo** p);

// Open \\.\PhysicalDriveN (write => also lock? no: caller decides).
HANDLE openPhysical(int diskNumber, bool write);
// Open a volume by NT device path or drive letter for raw sector I/O.
HANDLE openVolume(const std::wstring& pathOrLetter, bool write);

// Read/write `len` bytes at byte offset `offset` (must be sector-aligned).
bool readAt(HANDLE h, ULONGLONG offset, void* buf, DWORD len);
bool writeAt(HANDLE h, ULONGLONG offset, const void* buf, DWORD len);

// Lock + dismount a volume so the file system driver does not fight our raw writes.
bool lockVolume(HANDLE h, bool dismount);
void unlockVolume(HANDLE h);

std::wstring guidToString(const GUID& g);
const wchar_t* gptTypeName(const GUID& g);
const wchar_t* mbrTypeName(BYTE t);

}  // namespace disk
