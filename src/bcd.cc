#include "bcd.hh"
#include "log.hh"
#include "ntapi.hh"
#include <objbase.h>
#include <string.h>
#include <stdio.h>

#pragma comment(lib, "ole32.lib")

namespace bcd {

#define G(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
    const GUID name = { l, w1, w2, { b1, b2, b3, b4, b5, b6, b7, b8 } }
G(GUID_BOOTMGR,              0x9dea862c, 0x5cdd, 0x4e70, 0xac, 0xc1, 0xf3, 0x2b, 0x34, 0x4d, 0x47, 0x95);
G(GUID_FWBOOTMGR,            0xa5a30fa2, 0x3d06, 0x4e9f, 0xb5, 0xf4, 0xa0, 0x1d, 0xf9, 0xd1, 0xfc, 0xba);
G(GUID_MEMDIAG,              0xb2721d73, 0x1db4, 0x4c62, 0xbf, 0x78, 0xc5, 0x48, 0xa8, 0x80, 0x14, 0x2d);
G(GUID_NTLDR,                0x466f5a88, 0x0af2, 0x4f76, 0x90, 0x38, 0x09, 0x5b, 0x17, 0x0d, 0xc2, 0x1c);
G(GUID_GLOBALSETTINGS,       0x7ea2e1ac, 0x2e61, 0x4728, 0xaa, 0xa3, 0x89, 0x6d, 0x9d, 0x0a, 0x9f, 0x0e);
G(GUID_BOOTLOADERSETTINGS,   0x6efb52bf, 0x1766, 0x41db, 0xa6, 0xb3, 0x0e, 0xe5, 0xef, 0xf7, 0x2b, 0xd7);
G(GUID_RESUMELOADERSETTINGS, 0x1afa9c49, 0x16ab, 0x4a5c, 0x90, 0x1b, 0x21, 0x28, 0x02, 0xda, 0x94, 0x60);
G(GUID_DBGSETTINGS,          0x4636856e, 0x540f, 0x4170, 0xa1, 0x30, 0xa8, 0x47, 0x76, 0xf4, 0xc6, 0x54);
G(GUID_EMSSETTINGS,          0x0ce4991b, 0xe6b3, 0x4b16, 0xb2, 0x3c, 0x5e, 0x0d, 0x92, 0x50, 0xe5, 0xd9);
G(GUID_BADMEMORY,            0x5189b25c, 0x5558, 0x4bf2, 0xbc, 0xa4, 0x28, 0x9b, 0x11, 0xbd, 0x29, 0xe2);
G(GUID_HYPERVISORSETTINGS,   0x7ff607e0, 0x4395, 0x11db, 0xb0, 0xde, 0x08, 0x00, 0x20, 0x0c, 0x9a, 0x66);
G(GUID_RAMDISKOPTIONS,       0xae5534e0, 0xa924, 0x466c, 0xb8, 0x36, 0x75, 0x85, 0x39, 0xa3, 0xee, 0x3a);
#undef G

// ---------------------------------------------------------------------------
// GUID helpers

std::wstring guidString(const GUID& g)
{
    wchar_t buf[48];
    _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"{%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}",
                 g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
                 g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return buf;
}

bool parseGuid(const std::wstring& s, GUID* out)
{
    std::wstring t = s;
    if (!t.empty() && t[0] != L'{') t = L"{" + t + L"}";
    return CLSIDFromString(t.c_str(), out) == S_OK;
}

GUID newGuid()
{
    GUID g;
    CoCreateGuid(&g);
    return g;
}

std::wstring objectAlias(const GUID& g)
{
    struct { const GUID* g; const wchar_t* n; } known[] = {
        { &GUID_BOOTMGR, L"{bootmgr}" }, { &GUID_FWBOOTMGR, L"{fwbootmgr}" }, { &GUID_MEMDIAG, L"{memdiag}" },
        { &GUID_NTLDR, L"{ntldr}" }, { &GUID_GLOBALSETTINGS, L"{globalsettings}" },
        { &GUID_BOOTLOADERSETTINGS, L"{bootloadersettings}" }, { &GUID_RESUMELOADERSETTINGS, L"{resumeloadersettings}" },
        { &GUID_DBGSETTINGS, L"{dbgsettings}" }, { &GUID_EMSSETTINGS, L"{emssettings}" }, { &GUID_BADMEMORY, L"{badmemory}" },
        { &GUID_HYPERVISORSETTINGS, L"{hypervisorsettings}" }, { &GUID_RAMDISKOPTIONS, L"{ramdiskoptions}" },
    };
    for (auto& k : known)
        if (IsEqualGUID(*k.g, g)) return k.n;
    return L"";
}

const wchar_t* objectTypeName(DWORD type)
{
    switch (type) {
    case OBJ_FWBOOTMGR: return L"Firmware Boot Manager";
    case OBJ_BOOTMGR:   return L"Windows Boot Manager";
    case OBJ_NTLDR:     return L"Legacy OS Loader (ntldr)";
    case OBJ_OSLOADER:  return L"Windows Boot Loader";
    case OBJ_RESUME:    return L"Resume from Hibernate";
    case OBJ_MEMDIAG:   return L"Windows Memory Tester";
    case OBJ_BOOTAPP:   return L"Boot Application";
    case 0x101FFFFF:    return L"Firmware Application (101fffff)";
    default:
        if ((type >> 28) == 2) return L"Settings (inheritable)";
        if ((type >> 28) == 3) return L"Device Options";
        return L"Unknown";
    }
}

// ---------------------------------------------------------------------------
// Element names (bcdedit vocabulary).  Library elements are shared; the
// application-specific ones depend on the object type.

namespace {

struct E { DWORD id; const wchar_t* n; };
const E lib[] = {
        {0x11000001, L"device"}, {0x12000002, L"path"}, {0x12000004, L"description"}, {0x12000005, L"locale"},
        {0x14000006, L"inherit"}, {0x15000007, L"truncatememory"}, {0x14000008, L"recoverysequence"},
        {0x16000009, L"recoveryenabled"}, {0x1700000a, L"badmemorylist"}, {0x1600000b, L"badmemoryaccess"},
        {0x1500000c, L"firstmegabytepolicy"}, {0x1500000d, L"relocatephysical"}, {0x1500000e, L"avoidlowmemory"},
        {0x1600000f, L"traditionalkseg"}, {0x16000010, L"bootdebug"}, {0x15000011, L"debugtype"},
        {0x15000012, L"debugaddress"}, {0x15000013, L"debugport"}, {0x15000014, L"baudrate"}, {0x15000015, L"channel"},
        {0x12000016, L"targetname"}, {0x16000017, L"noumex"}, {0x15000018, L"debugstart"}, {0x12000019, L"busparams"},
        {0x1500001a, L"hostip"}, {0x1500001b, L"port"}, {0x1600001c, L"dhcp"}, {0x1200001d, L"key"}, {0x1600001e, L"vm"},
        {0x16000020, L"bootems"}, {0x15000022, L"emsport"}, {0x15000023, L"emsbaudrate"}, {0x12000030, L"loadoptions"},
        {0x16000040, L"advancedoptions"}, {0x16000041, L"optionsedit"}, {0x15000042, L"keyringaddress"},
        {0x11000043, L"bootstatdevice"}, {0x12000044, L"bootstatfilepath"}, {0x16000045, L"preservebootstat"},
        {0x16000046, L"graphicsmodedisabled"}, {0x15000047, L"configaccesspolicy"}, {0x16000048, L"nointegritychecks"},
        {0x16000049, L"testsigning"}, {0x1200004a, L"fontpath"}, {0x1500004b, L"integrityservices"},
        {0x1500004c, L"volumebandid"}, {0x16000050, L"extendedinput"}, {0x15000051, L"initialconsoleinput"},
        {0x15000052, L"graphicsresolution"}, {0x16000053, L"restartonfailure"}, {0x16000054, L"highestmode"},
        {0x16000060, L"isolatedcontext"}, {0x15000065, L"displaymessage"}, {0x15000066, L"displaymessageoverride"},
        {0x16000067, L"nobootuxlogo"}, {0x16000068, L"nobootuxtext"}, {0x16000069, L"nobootuxprogress"},
        {0x1600006a, L"nobootuxfade"}, {0x1600006b, L"bootuxreservepooldebug"}, {0x1600006c, L"bootuxdisabled"},
        {0x1500006d, L"bootuxfadeframes"}, {0x1600006e, L"bootuxdumpstats"}, {0x1600006f, L"bootuxshowstats"},
        {0x16000071, L"multibootsystem"}, {0x16000072, L"nokeyboard"}, {0x15000073, L"aliaswindowskey"},
        {0x16000074, L"bootshutdowndisabled"}, {0x15000075, L"performancefrequency"}, {0x15000076, L"securebootrawpolicy"},
        {0x17000077, L"allowedinmemorysettings"}, {0x15000079, L"bootuxtransitiontime"}, {0x1600007a, L"mobilegraphics"},
        {0x1600007b, L"forcefipscrypto"}, {0x1500007d, L"booterrorux"}, {0x1600007e, L"flightsigning"},
        {0x1500007f, L"measuredbootlogformat"}, {0x15000080, L"displayrotation"}, {0x15000081, L"logcontrol"},
        {0x16000082, L"nofirmwaresync"}, {0x11000084, L"windowssyspart"}, {0x16000087, L"numlock"},
    };
const E bootmgr[] = {
        // BcdBootMgrElementTypes
        {0x24000001, L"displayorder"}, {0x24000002, L"bootsequence"}, {0x23000003, L"default"}, {0x25000004, L"timeout"},
        {0x26000005, L"resume"}, {0x23000006, L"resumeobject"}, {0x24000007, L"startupsequence"},
        {0x24000010, L"toolsdisplayorder"}, {0x26000020, L"displaybootmenu"}, {0x26000021, L"noerrordisplay"},
        {0x21000022, L"bcddevice"}, {0x22000023, L"bcdfilepath"}, {0x26000024, L"hormenabled"}, {0x26000025, L"hiberroot"},
        {0x22000026, L"passwordoverride"}, {0x22000027, L"pinpassphraseoverride"}, {0x26000028, L"processcustomactionsfirst"},
        {0x27000030, L"customactions"}, {0x26000031, L"persistbootsequence"}, {0x26000032, L"skipstartupsequence"},
    };
const E osloader[] = {
        {0x21000001, L"osdevice"}, {0x22000002, L"systemroot"}, {0x23000003, L"resumeobject"}, {0x26000004, L"stampdisks"},
        {0x26000010, L"detecthal"}, {0x22000011, L"kernel"}, {0x22000012, L"hal"}, {0x22000013, L"dbgtransport"},
        {0x25000020, L"nx"}, {0x25000021, L"pae"}, {0x26000022, L"winpe"}, {0x26000024, L"nocrashautoreboot"},
        {0x26000025, L"lastknowngood"}, {0x26000026, L"oslnointegritychecks"}, {0x26000027, L"osltestsigning"},
        {0x26000030, L"nolowmem"}, {0x25000031, L"removememory"}, {0x25000032, L"increaseuserva"}, {0x25000033, L"perfmem"}, {0x26000040, L"vga"},
        {0x26000041, L"quietboot"}, {0x26000042, L"novesa"}, {0x26000043, L"novga"}, {0x25000050, L"clustermodeaddressing"},
        {0x26000051, L"usephysicaldestination"}, {0x25000052, L"restrictapiccluster"}, {0x22000053, L"evstore"},
        {0x26000054, L"uselegacyapicmode"}, {0x25000055, L"x2apicpolicy"}, {0x26000060, L"onecpu"}, {0x25000061, L"numproc"},
        {0x26000062, L"maxproc"}, {0x25000063, L"configflags"}, {0x26000064, L"maxgroup"}, {0x26000065, L"groupaware"},
        {0x25000066, L"groupsize"}, {0x26000070, L"usefirmwarepcisettings"}, {0x25000071, L"msi"}, {0x25000072, L"pciexpress"},
        {0x25000080, L"safeboot"}, {0x26000081, L"safebootalternateshell"}, {0x26000090, L"bootlog"}, {0x26000091, L"sos"},
        {0x260000a0, L"debug"}, {0x260000a1, L"halbreakpoint"}, {0x260000a2, L"useplatformclock"},
        {0x260000a3, L"forcelegacyplatform"}, {0x260000a4, L"useplatformtick"}, {0x260000a5, L"disabledynamictick"},
        {0x250000a6, L"tscsyncpolicy"}, {0x260000b0, L"ems"}, {0x250000c0, L"forcefailure"},
        {0x250000c1, L"driverloadfailurepolicy"}, {0x250000c2, L"bootmenupolicy"}, {0x260000c3, L"advancedoptions"},
        {0x260000c4, L"optionsedit"}, {0x250000e0, L"bootstatuspolicy"}, {0x260000e1, L"disableelamdrivers"},
        // BcdOSLoaderElementTypes, hypervisor block (verified against a live store: f3/f4/f5 = type/port/baudrate)
        {0x250000f0, L"hypervisorlaunchtype"}, {0x260000f2, L"hypervisordebug"}, {0x250000f3, L"hypervisordebugtype"},
        {0x250000f4, L"hypervisordebugport"}, {0x250000f5, L"hypervisorbaudrate"}, {0x250000f6, L"hypervisorchannel"},
        {0x250000f7, L"bootux"}, {0x220000f9, L"hypervisorbusparams"}, {0x250000fa, L"hypervisornumproc"},
        {0x250000fb, L"hypervisorrootprocpernode"}, {0x260000fc, L"hypervisoruselargevtlb"}, {0x250000fd, L"hypervisorhostip"},
        {0x250000fe, L"hypervisorhostport"}, {0x25000100, L"tpmbootentropy"}, {0x22000110, L"hypervisorusekey"},
        {0x22000112, L"hypervisorproductskutype"}, {0x25000113, L"hypervisorrootproc"}, {0x26000114, L"hypervisordhcp"},
        {0x25000115, L"hypervisoriommupolicy"}, {0x26000116, L"hypervisorusevapic"}, {0x22000117, L"hypervisorloadoptions"},
        {0x25000118, L"hypervisormsrfilterpolicy"}, {0x25000119, L"hypervisormmionxpolicy"}, {0x2500011a, L"hypervisorschedulertype"},
        {0x2500012b, L"xsavedisable"},
    };
const E resume[] = {
        {0x21000001, L"filedevice"}, {0x22000002, L"filepath"}, {0x26000003, L"customsettings"}, {0x26000004, L"pae"},
        {0x21000005, L"associatedosdevice"}, {0x26000006, L"debugoptionenabled"}, {0x25000007, L"bootux"},
        {0x25000008, L"bootmenupolicy"}, {0x26000024, L"hormenabled"},
    };
const E memdiag[] = {
        {0x25000001, L"passcount"}, {0x25000002, L"testmix"}, {0x25000003, L"failurecount"}, {0x25000004, L"testtofail"},
        {0x26000005, L"cacheenable"}, {0x26000006, L"failuresenabled"},
    };
const E device[] = {
        {0x35000001, L"ramdiskimageoffset"}, {0x35000002, L"ramdisktftpclientport"}, {0x31000003, L"ramdisksdidevice"},
        {0x32000004, L"ramdisksdipath"}, {0x35000005, L"ramdiskimagelength"}, {0x36000006, L"exportascd"},
        {0x35000007, L"ramdisktftpblocksize"}, {0x35000008, L"ramdisktftpwindowsize"}, {0x36000009, L"ramdiskmcenabled"},
        {0x3600000a, L"ramdiskmctftpfallback"}, {0x3600000b, L"ramdisktftpvarwindow"},
    };
const E ntldr[] = { {0x22000001, L"bpbstring"} };

// Application-class table for an object type.  Inheritable settings objects:
// 0x2010000x = bootmgr-class, 0x2020000x = osloader-class.
const E* appTable(DWORD objType, size_t* n)
{
    switch (objType) {
    case OBJ_FWBOOTMGR: case OBJ_BOOTMGR: *n = _countof(bootmgr); return bootmgr;
    case OBJ_OSLOADER: *n = _countof(osloader); return osloader;
    case OBJ_RESUME: *n = _countof(resume); return resume;
    case OBJ_MEMDIAG: *n = _countof(memdiag); return memdiag;
    case OBJ_NTLDR: *n = _countof(ntldr); return ntldr;
    default:
        if ((objType & 0xFFF00000) == 0x20100000) { *n = _countof(bootmgr); return bootmgr; }
        if ((objType & 0xFFF00000) == 0x20200000) { *n = _countof(osloader); return osloader; }
        if ((objType >> 28) == 3) { *n = _countof(device); return device; }
        *n = 0;
        return nullptr;
    }
}

const E* tableFor(DWORD id, DWORD objType, size_t* n)
{
    switch (id >> 28) {
    case 1: *n = _countof(lib); return lib;
    case 2: return appTable(objType, n);
    case 3: *n = _countof(device); return device;
    default: *n = 0; return nullptr;
    }
}

}  // namespace

std::wstring elementName(DWORD id, DWORD objType)
{
    size_t n;
    const E* tbl = tableFor(id, objType, &n);
    for (size_t i = 0; i < n; i++)
        if (tbl[i].id == id) return tbl[i].n;
    wchar_t buf[32];
    _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"custom:%08x", id);
    return buf;
}

bool elementIdFromName(const std::wstring& name, DWORD objType, DWORD* id)
{
    std::wstring s = name;
    if (_wcsnicmp(s.c_str(), L"custom:", 7) == 0) s = s.substr(7);
    wchar_t* end = nullptr;
    unsigned long v = wcstoul(s.c_str(), &end, 16);
    if (end && *end == 0 && s.size() >= 8) { *id = v; return true; }
    size_t appN = 0;
    const E* app = appTable(objType, &appN);
    for (size_t i = 0; i < _countof(lib); i++)
        if (_wcsicmp(lib[i].n, name.c_str()) == 0) { *id = lib[i].id; return true; }
    for (size_t i = 0; i < appN; i++)
        if (_wcsicmp(app[i].n, name.c_str()) == 0) { *id = app[i].id; return true; }
    return false;
}

bool objectTypeFromName(const std::wstring& name, DWORD* type)
{
    struct { const wchar_t* n; DWORD t; } known[] = {
        { L"fwbootmgr", OBJ_FWBOOTMGR }, { L"bootmgr", OBJ_BOOTMGR }, { L"osloader", OBJ_OSLOADER },
        { L"resume", OBJ_RESUME }, { L"memdiag", OBJ_MEMDIAG }, { L"ntldr", OBJ_NTLDR }, { L"bootapp", OBJ_BOOTAPP },
        { L"globalsettings", 0x20100000 }, { L"bootloadersettings", 0x20200003 }, { L"resumeloadersettings", 0x20200004 },
        { L"device", OBJ_DEVICE },
    };
    for (auto& k : known) if (_wcsicmp(k.n, name.c_str()) == 0) { *type = k.t; return true; }
    wchar_t* end = nullptr;
    unsigned long v = wcstoul(name.c_str(), &end, 16);
    if (end && *end == 0 && !name.empty()) { *type = v; return true; }
    return false;
}

bool parseObjectRef(const std::wstring& s, GUID* out)
{
    struct { const wchar_t* n; const GUID* g; } known[] = {
        { L"{bootmgr}", &GUID_BOOTMGR }, { L"{fwbootmgr}", &GUID_FWBOOTMGR }, { L"{memdiag}", &GUID_MEMDIAG },
        { L"{ntldr}", &GUID_NTLDR }, { L"{globalsettings}", &GUID_GLOBALSETTINGS },
        { L"{bootloadersettings}", &GUID_BOOTLOADERSETTINGS }, { L"{resumeloadersettings}", &GUID_RESUMELOADERSETTINGS },
        { L"{dbgsettings}", &GUID_DBGSETTINGS }, { L"{emssettings}", &GUID_EMSSETTINGS }, { L"{badmemory}", &GUID_BADMEMORY },
        { L"{hypervisorsettings}", &GUID_HYPERVISORSETTINGS }, { L"{ramdiskoptions}", &GUID_RAMDISKOPTIONS },
    };
    for (auto& k : known) if (_wcsicmp(k.n, s.c_str()) == 0) { *out = *k.g; return true; }
    return parseGuid(s, out);
}

// ---------------------------------------------------------------------------
// Device element encoding (BiCreatePartitionDevice / BiConvertElementToRegistryData)

std::vector<BYTE> encodeDevice(const Device& d)
{
    std::vector<BYTE> v(0x58, 0);
    memcpy(&v[0x00], &d.additionalOptions, 16);
    DWORD type = (DWORD)d.kind, size = 0x48;
    memcpy(&v[0x10], &type, 4);
    memcpy(&v[0x18], &size, 4);
    if (d.kind == Device::Partition) {
        if (d.gpt) {
            memcpy(&v[0x20], &d.gptPartitionId, 16);
            DWORD style = 0;  // GPT
            memcpy(&v[0x34], &style, 4);
            memcpy(&v[0x38], &d.gptDiskId, 16);
        } else {
            memcpy(&v[0x20], &d.mbrPartitionOffset, 8);
            DWORD style = 1;  // MBR
            memcpy(&v[0x34], &style, 4);
            memcpy(&v[0x38], &d.mbrDiskSignature, 4);
        }
    }
    return v;
}

bool decodeDevice(const BYTE* p, size_t len, Device* d)
{
    *d = Device();
    if (len < 0x20) return false;
    memcpy(&d->additionalOptions, p, 16);
    DWORD type, size;
    memcpy(&type, p + 0x10, 4);
    memcpy(&size, p + 0x18, 4);
    d->kind = (type == 0 || type == 5 || type == 6 || type == 8) ? (Device::Kind)type : Device::Unknown;
    if (type == Device::Local && len >= 0x24) {
        DWORD sub;
        memcpy(&sub, p + 0x20, 4);  // BL_LOCAL_DEVICE.Type
        d->local = (Device::LocalKind)sub;
    }
    if (type == Device::Partition && len >= 0x48) {
        DWORD style;
        memcpy(&style, p + 0x34, 4);
        d->gpt = style == 0;
        if (d->gpt) {
            memcpy(&d->gptPartitionId, p + 0x20, 16);
            memcpy(&d->gptDiskId, p + 0x38, 16);
        } else {
            memcpy(&d->mbrPartitionOffset, p + 0x20, 8);
            memcpy(&d->mbrDiskSignature, p + 0x38, 4);
        }
    }
    return true;
}

Device partitionDevice(const DiskInfo& dk, const PartitionInfo& p)
{
    Device d;
    d.kind = Device::Partition;
    d.gpt = dk.gpt;
    if (dk.gpt) {
        d.gptPartitionId = p.gptId;
        d.gptDiskId = dk.gptDiskId;
    } else {
        d.mbrPartitionOffset = p.startLba * dk.bytesPerSector;
        d.mbrDiskSignature = dk.mbrSignature;
    }
    return d;
}

Device bootDevice()
{
    Device d;
    d.kind = Device::Boot;
    return d;
}

std::wstring Device::describe() const
{
    wchar_t buf[256];
    GUID zero = {};
    std::wstring opts = IsEqualGUID(additionalOptions, zero) ? L"" : L" options=" + guidString(additionalOptions);
    switch (kind) {
    case Boot: return L"boot" + opts;
    case Locate: return L"locate" + opts;
    case Local:
        switch (local) {
        case Ramdisk: return L"ramdisk (parent device + path follow)" + opts;
        case File: return L"file (parent device + path follow)" + opts;
        case Floppy: return L"floppy" + opts;
        case CdRom: return L"cdrom" + opts;
        case VirtualDisk: return L"vhd" + opts;
        default: _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"local device sub-type %d", (int)local); return buf + opts;
        }
    case Partition:
        if (gpt)
            _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"partition GPT id=%s disk=%s", guidString(gptPartitionId).c_str(), guidString(gptDiskId).c_str());
        else
            _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"partition MBR offset=0x%I64X (%I64u MB) disk-sig=0x%08X", mbrPartitionOffset, mbrPartitionOffset >> 20, mbrDiskSignature);
        return buf + opts;
    default:
        _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"device type %d", (int)kind);
        return buf + opts;
    }
}

std::wstring describeElement(const Element& e, DWORD objType)
{
    std::wstring name = elementName(e.id, objType);
    wchar_t buf[128];
    std::wstring val;
    switch ((e.id >> 24) & 0xF) {
    case 1: {
        Device d;
        if (decodeDevice(e.data.data(), e.data.size(), &d)) val = d.describe();
        else val = L"(undecodable device)";
        break;
    }
    case 2: case 3:
        val.assign(reinterpret_cast<const wchar_t*>(e.data.data()), e.data.size() / 2);
        while (!val.empty() && val.back() == 0) val.pop_back();
        if (((e.id >> 24) & 0xF) == 3) { GUID g; if (parseGuid(val, &g)) { std::wstring a = objectAlias(g); if (!a.empty()) val += L" " + a; } }
        break;
    case 4: {
        const wchar_t* p = reinterpret_cast<const wchar_t*>(e.data.data());
        size_t n = e.data.size() / 2;
        for (size_t i = 0; i < n && p[i];) {
            std::wstring s(p + i);
            GUID g; std::wstring a;
            if (parseGuid(s, &g)) a = objectAlias(g);
            if (!val.empty()) val += L", ";
            val += a.empty() ? s : a;
            i += s.size() + 1;
        }
        break;
    }
    case 5: {
        ULONGLONG v = 0;
        memcpy(&v, e.data.data(), e.data.size() < 8 ? e.data.size() : 8);
        _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%I64u (0x%I64x)", v, v);
        val = buf;
        break;
    }
    case 6:
        val = (!e.data.empty() && e.data[0]) ? L"Yes" : L"No";
        break;
    case 7: {
        for (size_t i = 0; i + 8 <= e.data.size(); i += 8) {
            ULONGLONG v; memcpy(&v, &e.data[i], 8);
            _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%s0x%I64x", i ? L", " : L"", v);
            val += buf;
        }
        break;
    }
    default:
        _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"(%Iu bytes)", e.data.size());
        val = buf;
    }
    _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%-28s ", name.c_str());
    return buf + val;
}

std::wstring win32ToNtPath(const std::wstring& path)
{
    if (path.rfind(L"\\\\?\\", 0) == 0) return L"\\??\\" + path.substr(4);
    if (path.rfind(L"\\??\\", 0) == 0 || path.rfind(L"\\Device\\", 0) == 0) return path;
    return L"\\??\\" + path;
}

// ---------------------------------------------------------------------------
// Store

bool Store::loadHive(const std::wstring& ntPath, bool readOnly)
{
    if (!nt::enablePrivilege(SE_BACKUP_PRIVILEGE_ID, L"SeBackupPrivilege") ||
        !nt::enablePrivilege(SE_RESTORE_PRIVILEGE_ID, L"SeRestorePrivilege"))
        return false;
    // BiAddStoreFromFile: try BCD00000000, BCD00000001, ... until one loads.
    for (int n = 0; n < 32; n++) {
        wchar_t key[16];
        _snwprintf_s(key, _countof(key), _TRUNCATE, L"BCD%08d", n);
        HKEY probe;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_READ, &probe) == ERROR_SUCCESS) {
            RegCloseKey(probe);
            LOG_TRACE(L"HKLM\\%s is in use", key);
            continue;
        }
        NTSTATUS st = nt::loadHive(key, ntPath);
        if (NT_SUCCESS(st)) {
            m_keyName = key;
            m_loadedByUs = true;
            break;
        }
        if (st == (NTSTATUS)0xC0000043 /*SHARING_VIOLATION*/ || st == (NTSTATUS)0xC0000022 /*ACCESS_DENIED*/) {
            LOG_ERROR(L"the hive %s is already loaded (is it this machine's live system store? use the live store instead)", ntPath.c_str());
            return false;
        }
        if (st == STATUS_OBJECT_NAME_NOT_FOUND || st == (NTSTATUS)0xC000003A || st == (NTSTATUS)0xC000000F) {
            LOG_ERROR(L"BCD file not found: %s", ntPath.c_str());
            return false;
        }
        LOG_WARN(L"NtLoadKey(%s) failed 0x%08X, trying next slot", key, (unsigned)st);
    }
    if (!m_loadedByUs) return false;
    LONG rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, m_keyName.c_str(), 0, readOnly ? KEY_READ : KEY_READ | KEY_WRITE, &m_root);
    if (rc != ERROR_SUCCESS) {
        LOG_ERROR(L"RegOpenKeyEx(HKLM\\%s): %s", m_keyName.c_str(), logging::win32Error(rc).c_str());
        nt::unloadHive(m_keyName);
        m_loadedByUs = false;
        return false;
    }
    LOG_INFO(L"BCD store %s loaded at HKLM\\%s", ntPath.c_str(), m_keyName.c_str());
    return true;
}

bool Store::openFile(const std::wstring& path, bool readOnly)
{
    close();
    m_filePath = path;
    return loadHive(win32ToNtPath(path), readOnly);
}

bool Store::openLiveSystem(bool readOnly)
{
    close();
    LONG rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"BCD00000000", 0, readOnly ? KEY_READ : KEY_READ | KEY_WRITE, &m_root);
    if (rc != ERROR_SUCCESS) {
        LOG_ERROR(L"cannot open HKLM\\BCD00000000 (%s) - the live system store is not mounted or you are not elevated", logging::win32Error(rc).c_str());
        return false;
    }
    m_keyName = L"BCD00000000";
    m_filePath = L"(live system store)";
    LOG_INFO(L"opened the live system BCD store at HKLM\\BCD00000000");
    return true;
}

bool Store::createFile(const std::wstring& path)
{
    close();
    if (!nt::enablePrivilege(SE_BACKUP_PRIVILEGE_ID, L"SeBackupPrivilege") ||
        !nt::enablePrivilege(SE_RESTORE_PRIVILEGE_ID, L"SeRestorePrivilege"))
        return false;
    // BcdCreateStore: build an empty key under SYSTEM\CurrentControlSet\BootConfigurationData and
    // RegSaveKey it - that is a valid empty hive.  Then load it and add Objects + Description.
    const wchar_t* tmpPath = L"SYSTEM\\CurrentControlSet\\BootConfigurationData";
    HKEY tmp = NULL, newRoot = NULL;
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, tmpPath);
    LONG rc = RegCreateKeyExW(HKEY_LOCAL_MACHINE, tmpPath, 0, NULL, REG_OPTION_VOLATILE, KEY_ALL_ACCESS, NULL, &tmp, NULL);
    if (rc != ERROR_SUCCESS) { LOG_ERROR(L"cannot create temp key: %s", logging::win32Error(rc).c_str()); return false; }
    rc = RegCreateKeyExW(tmp, L"NewStoreRoot", 0, NULL, REG_OPTION_VOLATILE, KEY_ALL_ACCESS, NULL, &newRoot, NULL);
    if (rc != ERROR_SUCCESS) { LOG_ERROR(L"cannot create NewStoreRoot: %s", logging::win32Error(rc).c_str()); RegCloseKey(tmp); return false; }
    DeleteFileW(path.c_str());
    rc = RegSaveKeyExW(newRoot, path.c_str(), NULL, REG_LATEST_FORMAT);
    RegCloseKey(newRoot);
    RegCloseKey(tmp);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, tmpPath);
    if (rc != ERROR_SUCCESS) { LOG_ERROR(L"RegSaveKeyEx(%s): %s", path.c_str(), logging::win32Error(rc).c_str()); return false; }
    LOG_INFO(L"created empty hive %s", path.c_str());
    if (!openFile(path, false)) return false;

    HKEY k;
    if (RegCreateKeyExW(m_root, L"Objects", 0, NULL, 0, KEY_READ, NULL, &k, NULL) != ERROR_SUCCESS) return false;
    RegCloseKey(k);
    if (RegCreateKeyExW(m_root, L"Description", 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) != ERROR_SUCCESS) return false;
    RegSetValueExW(k, L"KeyName", 0, REG_SZ, reinterpret_cast<const BYTE*>(m_keyName.c_str()), (DWORD)(m_keyName.size() + 1) * 2);
    RegCloseKey(k);
    return true;
}

void Store::close()
{
    if (m_root) {
        RegFlushKey(m_root);
        RegCloseKey(m_root);
        m_root = NULL;
    }
    if (m_loadedByUs) {
        nt::unloadHive(m_keyName);
        m_loadedByUs = false;
        LOG_DEBUG(L"BCD store %s unloaded", m_filePath.c_str());
    }
    m_keyName.clear();
    m_filePath.clear();
}

bool Store::flush()
{
    LONG rc = RegFlushKey(m_root);
    if (rc != ERROR_SUCCESS) LOG_WARN(L"RegFlushKey: %s", logging::win32Error(rc).c_str());
    return rc == ERROR_SUCCESS;
}

bool Store::markAsSystemStore()
{
    HKEY k;
    if (RegCreateKeyExW(m_root, L"Description", 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) != ERROR_SUCCESS) return false;
    DWORD one = 1;
    LONG rc = RegSetValueExW(k, L"System", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one), 4);
    RegCloseKey(k);
    return rc == ERROR_SUCCESS;
}

HKEY Store::openObjectKey(const GUID& id, REGSAM sam)
{
    std::wstring path = L"Objects\\" + guidString(id);
    HKEY k = NULL;
    LONG rc = RegOpenKeyExW(m_root, path.c_str(), 0, sam, &k);
    if (rc != ERROR_SUCCESS) {
        LOG_TRACE(L"open object %s: %s", path.c_str(), logging::win32Error(rc).c_str());
        return NULL;
    }
    return k;
}

bool Store::objectExists(const GUID& id)
{
    HKEY k = openObjectKey(id, KEY_READ);
    if (!k) return false;
    RegCloseKey(k);
    return true;
}

bool Store::createObject(const GUID& id, DWORD type)
{
    std::wstring path = L"Objects\\" + guidString(id);
    HKEY obj, sub;
    LONG rc = RegCreateKeyExW(m_root, path.c_str(), 0, NULL, 0, KEY_ALL_ACCESS, NULL, &obj, NULL);
    if (rc != ERROR_SUCCESS) { LOG_ERROR(L"create %s: %s", path.c_str(), logging::win32Error(rc).c_str()); return false; }
    rc = RegCreateKeyExW(obj, L"Description", 0, NULL, 0, KEY_WRITE, NULL, &sub, NULL);
    if (rc == ERROR_SUCCESS) {
        rc = RegSetValueExW(sub, L"Type", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&type), 4);
        RegCloseKey(sub);
    }
    if (rc == ERROR_SUCCESS && RegCreateKeyExW(obj, L"Elements", 0, NULL, 0, KEY_READ, NULL, &sub, NULL) == ERROR_SUCCESS)
        RegCloseKey(sub);
    RegCloseKey(obj);
    if (rc != ERROR_SUCCESS) { LOG_ERROR(L"init %s: %s", path.c_str(), logging::win32Error(rc).c_str()); return false; }
    LOG_DEBUG(L"created object %s type 0x%08X (%s)", guidString(id).c_str(), type, objectTypeName(type));
    return true;
}

bool Store::deleteObject(const GUID& id)
{
    std::wstring path = L"Objects\\" + guidString(id);
    LONG rc = RegDeleteTreeW(m_root, path.c_str());
    if (rc != ERROR_SUCCESS) { LOG_ERROR(L"delete %s: %s", path.c_str(), logging::win32Error(rc).c_str()); return false; }
    LOG_DEBUG(L"deleted object %s", guidString(id).c_str());
    return true;
}

bool Store::readObject(const GUID& id, Object* out)
{
    out->id = id;
    out->type = 0;
    out->elements.clear();
    HKEY obj = openObjectKey(id, KEY_READ);
    if (!obj) return false;
    HKEY desc;
    if (RegOpenKeyExW(obj, L"Description", 0, KEY_READ, &desc) == ERROR_SUCCESS) {
        DWORD len = 4, t;
        RegQueryValueExW(desc, L"Type", NULL, &t, reinterpret_cast<BYTE*>(&out->type), &len);
        RegCloseKey(desc);
    }
    HKEY elems;
    if (RegOpenKeyExW(obj, L"Elements", 0, KEY_READ, &elems) == ERROR_SUCCESS) {
        for (DWORD i = 0;; i++) {
            wchar_t name[64];
            DWORD nameLen = _countof(name);
            if (RegEnumKeyExW(elems, i, name, &nameLen, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
            Element e;
            e.id = wcstoul(name, NULL, 16);
            HKEY ek;
            if (RegOpenKeyExW(elems, name, 0, KEY_READ, &ek) != ERROR_SUCCESS) continue;
            DWORD len = 0;
            if (RegQueryValueExW(ek, L"Element", NULL, &e.regType, NULL, &len) == ERROR_SUCCESS) {
                e.data.resize(len);
                if (len && RegQueryValueExW(ek, L"Element", NULL, &e.regType, e.data.data(), &len) != ERROR_SUCCESS)
                    e.data.clear();
            }
            RegCloseKey(ek);
            out->elements.push_back(e);
        }
        RegCloseKey(elems);
    }
    RegCloseKey(obj);
    return true;
}

bool Store::listObjects(std::vector<Object>* out)
{
    HKEY objs;
    LONG rc = RegOpenKeyExW(m_root, L"Objects", 0, KEY_READ, &objs);
    if (rc != ERROR_SUCCESS) { LOG_ERROR(L"no Objects key in store: %s", logging::win32Error(rc).c_str()); return false; }
    for (DWORD i = 0;; i++) {
        wchar_t name[64];
        DWORD nameLen = _countof(name);
        if (RegEnumKeyExW(objs, i, name, &nameLen, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        GUID g;
        if (!parseGuid(name, &g)) { LOG_WARN(L"object key '%s' is not a GUID", name); continue; }
        Object o;
        if (readObject(g, &o)) out->push_back(o);
    }
    RegCloseKey(objs);
    return true;
}

bool Store::setElement(const GUID& obj, DWORD elem, DWORD regType, const void* data, DWORD len)
{
    HKEY ok = openObjectKey(obj, KEY_WRITE | KEY_READ);
    if (!ok) { LOG_ERROR(L"object %s does not exist", guidString(obj).c_str()); return false; }
    wchar_t path[64];
    _snwprintf_s(path, _countof(path), _TRUNCATE, L"Elements\\%08x", elem);
    HKEY ek;
    LONG rc = RegCreateKeyExW(ok, path, 0, NULL, 0, KEY_WRITE, NULL, &ek, NULL);
    if (rc == ERROR_SUCCESS) {
        rc = RegSetValueExW(ek, L"Element", 0, regType, static_cast<const BYTE*>(data), len);
        RegCloseKey(ek);
    }
    RegCloseKey(ok);
    if (rc != ERROR_SUCCESS) {
        LOG_ERROR(L"set %s\\%s: %s", guidString(obj).c_str(), path, logging::win32Error(rc).c_str());
        return false;
    }
    LOG_TRACE(L"set %s %08x (%lu bytes, reg type %lu)", guidString(obj).c_str(), elem, len, regType);
    return true;
}

bool Store::setBinary(const GUID& obj, DWORD elem, const void* data, DWORD len) { return setElement(obj, elem, REG_BINARY, data, len); }
bool Store::setString(const GUID& obj, DWORD elem, const std::wstring& s) { return setElement(obj, elem, REG_SZ, s.c_str(), (DWORD)(s.size() + 1) * 2); }
bool Store::setGuid(const GUID& obj, DWORD elem, const GUID& g) { return setString(obj, elem, guidString(g)); }
bool Store::setInteger(const GUID& obj, DWORD elem, ULONGLONG v) { return setElement(obj, elem, REG_BINARY, &v, 8); }
bool Store::setBoolean(const GUID& obj, DWORD elem, bool v) { BYTE b = v ? 1 : 0; return setElement(obj, elem, REG_BINARY, &b, 1); }
bool Store::setDevice(const GUID& obj, DWORD elem, const Device& d) { std::vector<BYTE> v = encodeDevice(d); return setElement(obj, elem, REG_BINARY, v.data(), (DWORD)v.size()); }

bool Store::setGuidList(const GUID& obj, DWORD elem, const std::vector<GUID>& list)
{
    std::wstring multi;
    for (const GUID& g : list) { multi += guidString(g); multi.push_back(0); }
    multi.push_back(0);
    return setElement(obj, elem, REG_MULTI_SZ, multi.c_str(), (DWORD)multi.size() * 2);
}

bool Store::getGuidList(const GUID& obj, DWORD elem, std::vector<GUID>* out)
{
    Object o;
    if (!readObject(obj, &o)) return false;
    for (const Element& e : o.elements) {
        if (e.id != elem) continue;
        const wchar_t* p = reinterpret_cast<const wchar_t*>(e.data.data());
        size_t n = e.data.size() / 2;
        for (size_t i = 0; i < n && p[i];) {
            std::wstring s(p + i);
            GUID g;
            if (parseGuid(s, &g)) out->push_back(g);
            i += s.size() + 1;
        }
        return true;
    }
    return false;
}

bool Store::deleteElement(const GUID& obj, DWORD elem)
{
    HKEY ok = openObjectKey(obj, KEY_WRITE | KEY_READ);
    if (!ok) return false;
    wchar_t path[64];
    _snwprintf_s(path, _countof(path), _TRUNCATE, L"Elements\\%08x", elem);
    LONG rc = RegDeleteTreeW(ok, path);
    RegCloseKey(ok);
    return rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND;
}

bool Store::exportTo(const std::wstring& path)
{
    nt::enablePrivilege(SE_BACKUP_PRIVILEGE_ID, L"SeBackupPrivilege");
    RegFlushKey(m_root);
    DeleteFileW(path.c_str());
    LONG rc = RegSaveKeyExW(m_root, path.c_str(), NULL, REG_LATEST_FORMAT);
    if (rc != ERROR_SUCCESS) { LOG_ERROR(L"RegSaveKeyEx(%s): %s", path.c_str(), logging::win32Error(rc).c_str()); return false; }
    LOG_INFO(L"store exported to %s", path.c_str());
    return true;
}

}  // namespace bcd
