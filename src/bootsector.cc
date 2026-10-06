#include "bootsector.hh"
#include "bootcode_data.hh"
#include "log.hh"
#include <string.h>

namespace {

bool hasAscii(const BYTE* p, size_t len, const char* s)
{
    size_t n = strlen(s);
    for (size_t i = 0; i + n <= len; i++)
        if (memcmp(p + i, s, n) == 0)
            return true;
    return false;
}

bool hasUtf16(const BYTE* p, size_t len, const char* s)
{
    size_t n = strlen(s);
    for (size_t i = 0; i + 2 * n <= len; i++) {
        bool ok = true;
        for (size_t k = 0; k < n && ok; k++)
            ok = p[i + 2 * k] == (BYTE)s[k] && p[i + 2 * k + 1] == 0;
        if (ok) return true;
    }
    return false;
}

// Compare the code region (after the BPB) of `sector` with the blobs.
CodeKind matchCode(const BYTE* sector, size_t bpbLen, const unsigned char* nt60, const unsigned char* nt52, size_t compareLen)
{
    if (bpbLen >= compareLen) return CODE_OTHER;
    if (memcmp(sector + bpbLen, nt60 + bpbLen, compareLen - bpbLen) == 0) return CODE_NT60;
    if (memcmp(sector + bpbLen, nt52 + bpbLen, compareLen - bpbLen) == 0) return CODE_NT52;
    bool zero = true;
    for (size_t i = bpbLen; i < compareLen && zero; i++) zero = sector[i] == 0;
    return zero ? CODE_NONE : CODE_OTHER;
}

}  // namespace

namespace bootsector {

const wchar_t* fsName(FsKind k)
{
    switch (k) {
    case FS_FAT:   return L"FAT12/16";
    case FS_FAT32: return L"FAT32";
    case FS_NTFS:  return L"NTFS";
    case FS_EXFAT: return L"exFAT";
    default:       return L"unknown";
    }
}

const wchar_t* codeName(CodeKind k)
{
    switch (k) {
    case CODE_NT60:  return L"Windows Vista+ (BOOTMGR)";
    case CODE_NT52:  return L"Windows XP/2003 (NTLDR)";
    case CODE_NONE:  return L"none (zeroed)";
    default:         return L"other/unknown";
    }
}

BootSectorInfo inspect(const BYTE* s, size_t len)
{
    BootSectorInfo bi;
    if (len < 512) return bi;
    bi.validSignature = s[0x1FE] == 0x55 && s[0x1FF] == 0xAA;
    bi.oem.assign(reinterpret_cast<const char*>(s + 3), 8);
    bool jmp = s[0] == 0xEB && s[2] == 0x90;

    if (memcmp(s + 3, "NTFS    ", 8) == 0 || memcmp(s + 3, "-FVE-FS-", 8) == 0) {
        // Same validation as CDisk::IsBootSectorNtfs: zero reserved fields, sane cluster sizes.
        bi.fs = FS_NTFS;
        bi.bytesPerSector = *reinterpret_cast<const WORD*>(s + 0x0B);
        bi.totalSectors = *reinterpret_cast<const ULONGLONG*>(s + 0x28);
        bi.bpbLength = 0x54;
        bi.code = matchCode(s, 0x54, bootcode::nt60_ntfs, bootcode::nt52_ntfs, 0x200);
    } else if (memcmp(s + 3, "EXFAT   ", 8) == 0) {
        bi.fs = FS_EXFAT;
        bi.bytesPerSector = 1u << s[0x6C];
        bi.totalSectors = *reinterpret_cast<const ULONGLONG*>(s + 0x48);
        bi.bpbLength = (size_t)s[1] + 2;
        bi.code = matchCode(s, bi.bpbLength, bootcode::nt60_exfat, bootcode::nt60_exfat, 0x200);
        if (bi.code == CODE_NT52) bi.code = CODE_NT60;  // same blob passed twice
    } else if (jmp && memcmp(s + 0x52, "FAT32   ", 8) == 0) {
        bi.fs = FS_FAT32;
        bi.bytesPerSector = *reinterpret_cast<const WORD*>(s + 0x0B);
        bi.totalSectors = *reinterpret_cast<const DWORD*>(s + 0x20);
        bi.bpbLength = (size_t)s[1] + 2;
        bi.code = matchCode(s, bi.bpbLength, bootcode::nt60_fat32, bootcode::nt52_fat32, 0x200);
    } else if (jmp && memcmp(s + 0x36, "FAT", 3) == 0) {
        bi.fs = FS_FAT;
        bi.bytesPerSector = *reinterpret_cast<const WORD*>(s + 0x0B);
        bi.totalSectors = *reinterpret_cast<const WORD*>(s + 0x13);
        if (!bi.totalSectors) bi.totalSectors = *reinterpret_cast<const DWORD*>(s + 0x20);
        bi.bpbLength = (size_t)s[1] + 2;
        bi.code = matchCode(s, bi.bpbLength, bootcode::nt60_fat, bootcode::nt52_fat, 0x200);
    }
    bi.mentionsBootmgr = hasAscii(s, len, "BOOTMGR") || hasUtf16(s, len, "BOOTMGR");
    bi.mentionsNtldr = hasAscii(s, len, "NTLDR") || hasUtf16(s, len, "NTLDR");
    return bi;
}

MbrInfo inspectMbr(const BYTE* s, size_t len)
{
    MbrInfo m;
    if (len < 512) return m;
    m.validSignature = s[0x1FE] == 0x55 && s[0x1FF] == 0xAA;
    m.diskSignature = *reinterpret_cast<const DWORD*>(s + 0x1B8);
    m.allZero = true;
    for (size_t i = 0; i < 512 && m.allZero; i++) m.allZero = s[i] == 0;
    int used = 0;
    for (int i = 0; i < 4; i++) {
        const BYTE* e = s + 0x1BE + i * 16;
        if (e[4] != 0) used++;
        if (e[0] == 0x80) m.activeCount++;
    }
    m.protectiveGpt = used == 1 && s[0x1BE + 4] == 0xEE;

    if (memcmp(s, bootcode::nt60_mbr, 0x1B8) == 0) { m.code = CODE_NT60; m.codeName = L"Windows Vista+ MBR"; }
    else if (memcmp(s, bootcode::nt52_mbr, 0x1B8) == 0) { m.code = CODE_NT52; m.codeName = L"Windows XP/2003 MBR"; }
    else {
        bool zero = true;
        for (size_t i = 0; i < 0x1B8 && zero; i++) zero = s[i] == 0;
        if (zero) { m.code = CODE_NONE; m.codeName = L"no boot code"; }
        else {
            m.code = CODE_OTHER;
            if (hasAscii(s, 0x1B8, "GRUB")) m.codeName = L"GRUB";
            else if (hasAscii(s, 0x1B8, "LILO")) m.codeName = L"LILO";
            else if (hasAscii(s, 0x1B8, "Invalid partition table")) m.codeName = L"Windows MBR (other build)";
            else if (hasAscii(s, 0x1B8, "SYSLINUX")) m.codeName = L"SYSLINUX";
            else m.codeName = L"unknown";
        }
    }
    return m;
}

void buildMbr(bool nt52, const BYTE* current, BYTE* out)
{
    memcpy(out, current, 512);
    memcpy(out, nt52 ? bootcode::nt52_mbr : bootcode::nt60_mbr, 0x1B8);  // keep signature + table
    out[0x1FE] = 0x55;
    out[0x1FF] = 0xAA;
}

bool buildVolumeBootCode(FsKind fs, bool nt52, DWORD bps, const BYTE* cur, size_t curLen,
                         std::string* out, std::wstring* why)
{
    if (bps < 512 || (bps & (bps - 1))) { *why = L"unsupported sector size"; return false; }
    const unsigned char* blob = nullptr;
    size_t blobLen = 0, keep = 0;
    switch (fs) {
    case FS_FAT:   blob = nt52 ? bootcode::nt52_fat : bootcode::nt60_fat;     blobLen = 0x200;  keep = (size_t)cur[1] + 2; break;
    case FS_FAT32: blob = nt52 ? bootcode::nt52_fat32 : bootcode::nt60_fat32; blobLen = 0x600;  keep = (size_t)cur[1] + 2; break;
    case FS_NTFS:  blob = nt52 ? bootcode::nt52_ntfs : bootcode::nt60_ntfs;   blobLen = 0x2000; keep = 0x54; break;
    case FS_EXFAT:
        if (nt52) { *why = L"NT52 (NTLDR) boot code does not exist for exFAT"; return false; }
        blob = bootcode::nt60_exfat; blobLen = 0x600; keep = (size_t)cur[1] + 2; break;
    default:
        *why = L"file system not recognised"; return false;
    }
    if (keep >= 0x200 || keep < 3) { *why = L"implausible BPB length in the current boot sector"; return false; }
    if (fs == FS_FAT32 && bps != 512) { *why = L"bootsect only supports 512-byte sectors for FAT32"; return false; }

    if (fs == FS_EXFAT) {
        // 12 boot sectors + 12 backup, checksum sector 11 (bootsect: BootsectUpdateVolumeBootcode).
        size_t total = (size_t)bps * 24;
        if (curLen < total) { *why = L"need 24 sectors of current data for exFAT"; return false; }
        std::string img(reinterpret_cast<const char*>(cur), total);
        memcpy(&img[keep], blob + keep, 0x200 - keep);
        memcpy(&img[bps], blob + 0x200, 0x200);
        memcpy(&img[2 * (size_t)bps], blob + 0x400, 0x200);
        DWORD cs = 0;
        size_t n = (size_t)bps * 11;
        for (size_t i = 0; i < n; i++) {
            if (i == 106 || i == 107 || i == 112) continue;  // VolumeFlags and PercentInUse are excluded
            cs = ((cs << 31) | (cs >> 1)) + (BYTE)img[i];
        }
        for (size_t i = n; i < (size_t)bps * 12; i += 4)
            memcpy(&img[i], &cs, 4);
        memcpy(&img[(size_t)bps * 12], &img[0], (size_t)bps * 12);
        *out = img;
        return true;
    }

    // FAT/FAT32/NTFS: one sector of current data, overlay the blob after the BPB.
    size_t total = blobLen > bps ? blobLen : bps;
    std::string img(total, '\0');
    memcpy(&img[0], cur, bps < curLen ? bps : curLen);
    memcpy(&img[keep], blob + keep, blobLen - keep);
    if (fs == FS_NTFS && nt52)
        img[0x24] = static_cast<char>(static_cast<unsigned char>(0x80));  // XP boot code wants the BIOS drive number in the BPB
    *out = img;
    return true;
}

}  // namespace bootsector

std::wstring BootSectorInfo::describe() const
{
    wchar_t buf[256];
    _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%s, OEM '%S', %lu B/sector, %I64u sectors, sig %s, boot code: %s%s%s",
                 bootsector::fsName(fs), oem.c_str(), bytesPerSector, totalSectors, validSignature ? L"ok" : L"MISSING",
                 bootsector::codeName(code), mentionsBootmgr ? L" [BOOTMGR]" : L"", mentionsNtldr ? L" [NTLDR]" : L"");
    return buf;
}

std::wstring MbrInfo::describe() const
{
    wchar_t buf[256];
    _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"sig %s, disk id 0x%08X, code: %s, %d active, %s",
                 validSignature ? L"ok" : L"MISSING", diskSignature, codeName.c_str(), activeCount,
                 protectiveGpt ? L"GPT protective" : allZero ? L"EMPTY" : L"MBR table");
    return buf;
}
