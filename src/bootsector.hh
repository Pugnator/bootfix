// Boot sector / MBR inspection shared by `scan` and the fix commands.
#pragma once
#include <windows.h>
#include <string>

enum FsKind { FS_UNKNOWN = 0, FS_FAT = 1, FS_FAT32 = 2, FS_NTFS = 3, FS_EXFAT = 4 };
enum CodeKind { CODE_NONE = 0, CODE_NT60 = 1, CODE_NT52 = 2, CODE_OTHER = 3 };

struct BootSectorInfo {
    FsKind   fs = FS_UNKNOWN;
    std::string oem;              // 8-byte OEM id as text
    DWORD    bytesPerSector = 0;  // from BPB (0 when not a known layout)
    ULONGLONG totalSectors = 0;   // from BPB
    bool     validSignature = false;  // 0x55AA at 0x1FE
    size_t   bpbLength = 0;       // bytes preserved by bootsect when rewriting (jump target)
    CodeKind code = CODE_NONE;    // which Windows boot code the sector carries
    bool     mentionsBootmgr = false, mentionsNtldr = false;
    std::wstring describe() const;
};

struct MbrInfo {
    bool validSignature = false;
    DWORD diskSignature = 0;
    CodeKind code = CODE_NONE;
    bool allZero = false;
    bool protectiveGpt = false;   // single 0xEE entry
    int  activeCount = 0;
    std::wstring codeName;        // e.g. "Windows 7+ (BOOTMGR)", "GRUB", "unknown"
    std::wstring describe() const;
};

namespace bootsector {

// Identify the file system and boot code of a volume boot sector (first sector only).
BootSectorInfo inspect(const BYTE* sector, size_t len);
// Identify the boot code in an MBR (first 0x1B8 bytes) and basic table facts.
MbrInfo inspectMbr(const BYTE* sector, size_t len);

const wchar_t* fsName(FsKind k);
const wchar_t* codeName(CodeKind k);

// Build the image that bootsect would write for this volume: `out` receives the
// full byte run starting at sector 0 (size depends on fs), `current` is the
// current first sector(s) (at least one sector; 24 sectors for exFAT).
// Returns false with `why` set when the combination is unsupported.
bool buildVolumeBootCode(FsKind fs, bool nt52, DWORD bytesPerSector,
                         const BYTE* current, size_t currentLen,
                         std::string* out, std::wstring* why);

// Build the new MBR: boot code from the blob, signature + table from `current`.
void buildMbr(bool nt52, const BYTE* current, BYTE* out512);

}  // namespace bootsector
