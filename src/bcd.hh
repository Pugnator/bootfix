// BCD store access.  A BCD store is a registry hive file; the original
// tools statically link "bcd.lib" which loads it under HKLM\BCD0000000N
// (BiLoadHive / BiAddStoreFromFile) and edits plain registry keys:
//
//   <store>\Description            KeyName (REG_SZ), System (REG_DWORD), ...
//   <store>\Objects\{guid}\Description\Type   (REG_DWORD, e.g. 0x10200003 = osloader)
//   <store>\Objects\{guid}\Elements\<%08x>\Element
//
// Element registry types (BcdSetElementDataWithFlags): format nibble
// (type >> 24 & 0xF): 1 device, 5 integer, 6 boolean, 7 integer-list -> REG_BINARY;
// 2 string, 3 object -> REG_SZ; 4 object-list -> REG_MULTI_SZ.
#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include "disk.hh"

namespace bcd {

// Well-known object GUIDs (bcdedit aliases).
extern const GUID GUID_BOOTMGR;          // {bootmgr}
extern const GUID GUID_FWBOOTMGR;        // {fwbootmgr}
extern const GUID GUID_MEMDIAG;          // {memdiag}
extern const GUID GUID_NTLDR;            // {ntldr}
extern const GUID GUID_GLOBALSETTINGS;   // {globalsettings}
extern const GUID GUID_BOOTLOADERSETTINGS;
extern const GUID GUID_RESUMELOADERSETTINGS;
extern const GUID GUID_DBGSETTINGS;
extern const GUID GUID_EMSSETTINGS;
extern const GUID GUID_BADMEMORY;
extern const GUID GUID_HYPERVISORSETTINGS;
extern const GUID GUID_RAMDISKOPTIONS;

enum ObjectType {
    OBJ_FWBOOTMGR = 0x10100001,
    OBJ_BOOTMGR   = 0x10100002,
    OBJ_NTLDR     = 0x10300006,
    OBJ_OSLOADER  = 0x10200003,
    OBJ_RESUME    = 0x10200004,
    OBJ_MEMDIAG   = 0x10200005,
    OBJ_BOOTAPP   = 0x10200008,  // generic boot application
    OBJ_SETTINGS  = 0x20100000,  // inheritable settings (low bits vary)
    OBJ_DEVICE    = 0x30000000,
};

// Element ids used by the repair logic (library class 0x1, bootmgr 0x2x, osloader 0x2x).
enum ElementId {
    E_DEVICE            = 0x11000001,
    E_PATH              = 0x12000002,
    E_DESCRIPTION       = 0x12000004,
    E_LOCALE            = 0x12000005,
    E_INHERIT           = 0x14000006,
    E_RECOVERYSEQUENCE  = 0x14000008,
    E_RECOVERYENABLED   = 0x16000009,
    E_BM_DISPLAYORDER   = 0x24000001,
    E_BM_BOOTSEQUENCE   = 0x24000002,
    E_BM_DEFAULT        = 0x23000003,
    E_BM_TIMEOUT        = 0x25000004,
    E_BM_RESUMEOBJECT   = 0x23000006,
    E_BM_TOOLSDISPLAYORDER = 0x24000010,
    E_BM_DISPLAYBOOTMENU = 0x26000020,
    E_OS_OSDEVICE       = 0x21000001,
    E_OS_SYSTEMROOT     = 0x22000002,
    E_OS_RESUMEOBJECT   = 0x23000003,
    E_OS_WINPE          = 0x26000022,
    E_OS_BOOTMENUPOLICY = 0x250000c2,
    E_OS_NX             = 0x25000020,
    E_RESUME_FILEDEVICE = 0x21000001,
    E_RESUME_FILEPATH   = 0x22000002,
    E_RESUME_ASSOCIATEDOSDEVICE = 0x21000005,
    E_RESUME_BOOTMENUPOLICY = 0x25000008,
};

// Decoded "boot environment device" (BiCreatePartitionDevice output, 0x48 bytes after the 16-byte options GUID).
struct Device {
    enum Kind { Local = 0, Boot = 5, Partition = 6, Locate = 8, Unknown = 0xFF } kind = Unknown;
    // For Local: the local device sub-type (BiConvertNtDeviceToBootEnvironment: 3 = ramdisk, 5 = file).
    enum LocalKind { LocalDisk = 0, Floppy = 1, CdRom = 2, Ramdisk = 3, File = 5, VirtualDisk = 6 } local = LocalDisk;
    bool gpt = false;
    ULONGLONG mbrPartitionOffset = 0;  // MBR: byte offset of the partition on the disk
    DWORD mbrDiskSignature = 0;
    GUID gptPartitionId = {};
    GUID gptDiskId = {};
    GUID additionalOptions = {};
    std::wstring describe() const;
};

std::vector<BYTE> encodeDevice(const Device& d);               // -> 0x58 bytes
bool decodeDevice(const BYTE* data, size_t len, Device* out);
Device partitionDevice(const DiskInfo& d, const PartitionInfo& p);
Device bootDevice();

struct Element {
    DWORD id = 0;
    DWORD regType = 0;
    std::vector<BYTE> data;
};

struct Object {
    GUID id = {};
    DWORD type = 0;
    std::vector<Element> elements;
};

class Store {
public:
    Store() {}
    ~Store() { close(); }

    // Load a BCD hive file (Win32 path, drive letter or \\?\GLOBALROOT form) under HKLM\BCD0000000N.
    bool openFile(const std::wstring& path, bool readOnly);
    // Open the live system store (HKLM\BCD00000000) that Windows mounts at boot on this machine.
    bool openLiveSystem(bool readOnly);
    // Create a brand new empty store file (BiCreateHive: save an empty key) and open it.
    bool createFile(const std::wstring& path);
    void close();

    bool isOpen() const { return m_root != NULL; }
    const std::wstring& keyName() const { return m_keyName; }
    const std::wstring& filePath() const { return m_filePath; }

    bool listObjects(std::vector<Object>* out);   // with elements
    bool readObject(const GUID& id, Object* out);
    bool createObject(const GUID& id, DWORD type);  // BiCreateObject: Description\Type + Elements
    bool deleteObject(const GUID& id);
    bool objectExists(const GUID& id);

    bool setBinary(const GUID& obj, DWORD elem, const void* data, DWORD len);
    bool setString(const GUID& obj, DWORD elem, const std::wstring& s);
    bool setGuid(const GUID& obj, DWORD elem, const GUID& g);
    bool setGuidList(const GUID& obj, DWORD elem, const std::vector<GUID>& list);
    bool setInteger(const GUID& obj, DWORD elem, ULONGLONG v);
    bool setBoolean(const GUID& obj, DWORD elem, bool v);
    bool setDevice(const GUID& obj, DWORD elem, const Device& d);
    bool getGuidList(const GUID& obj, DWORD elem, std::vector<GUID>* out);
    bool deleteElement(const GUID& obj, DWORD elem);

    bool markAsSystemStore();   // Description\System = 1
    bool flush();
    bool exportTo(const std::wstring& path);   // bcdedit /export (RegSaveKey of the loaded hive)

private:
    bool loadHive(const std::wstring& ntPath, bool readOnly);
    HKEY openObjectKey(const GUID& id, REGSAM sam);
    bool setElement(const GUID& obj, DWORD elem, DWORD regType, const void* data, DWORD len);

    HKEY m_root = NULL;
    std::wstring m_keyName, m_filePath;
    bool m_loadedByUs = false;
};

std::wstring guidString(const GUID& g);            // {xxxxxxxx-...}
bool parseGuid(const std::wstring& s, GUID* out);
std::wstring objectAlias(const GUID& g);           // "{bootmgr}" or "" if not well known
const wchar_t* objectTypeName(DWORD type);
std::wstring elementName(DWORD id, DWORD objectType);
bool elementIdFromName(const std::wstring& name, DWORD objectType, DWORD* id);  // "osdevice" | "custom:21000001" | "0x21000001"
bool objectTypeFromName(const std::wstring& name, DWORD* type);                  // "osloader" | "bootmgr" | ... | hex
bool parseObjectRef(const std::wstring& s, GUID* out);                           // "{bootmgr}" alias or "{guid}"
std::wstring describeElement(const Element& e, DWORD objectType);
GUID newGuid();

// Convert a Win32 path to the NT form NtLoadKey wants.
std::wstring win32ToNtPath(const std::wstring& path);

}  // namespace bcd
