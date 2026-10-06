// Thin native-API layer.  The original tools (bootrec/bcdboot/bootsect) use
// ntdll directly for object-directory walks, symbolic links and BCD hive
// loading; we need the same handful of calls.
#pragma once
#include <windows.h>
#include <winternl.h>
#include <string>
#include <vector>

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS)0)
#endif
#ifndef NT_SUCCESS
#define NT_SUCCESS(s) (((NTSTATUS)(s)) >= 0)
#endif
#ifndef STATUS_MORE_ENTRIES
#define STATUS_MORE_ENTRIES ((NTSTATUS)0x00000105L)
#endif
#ifndef STATUS_NO_MORE_ENTRIES
#define STATUS_NO_MORE_ENTRIES ((NTSTATUS)0x8000001AL)
#endif
#ifndef STATUS_OBJECT_NAME_NOT_FOUND
#define STATUS_OBJECT_NAME_NOT_FOUND ((NTSTATUS)0xC0000034L)
#endif

extern "C" {
NTSTATUS NTAPI RtlAdjustPrivilege(ULONG privilege, BOOLEAN enable, BOOLEAN currentThread, PBOOLEAN wasEnabled);
NTSTATUS NTAPI NtOpenDirectoryObject(PHANDLE h, ACCESS_MASK access, POBJECT_ATTRIBUTES oa);
NTSTATUS NTAPI NtQueryDirectoryObject(HANDLE h, PVOID buf, ULONG len, BOOLEAN single, BOOLEAN restart, PULONG ctx, PULONG retLen);
NTSTATUS NTAPI NtOpenSymbolicLinkObject(PHANDLE h, ACCESS_MASK access, POBJECT_ATTRIBUTES oa);
NTSTATUS NTAPI NtQuerySymbolicLinkObject(HANDLE h, PUNICODE_STRING target, PULONG retLen);
NTSTATUS NTAPI NtLoadKey(POBJECT_ATTRIBUTES targetKey, POBJECT_ATTRIBUTES sourceFile);
NTSTATUS NTAPI NtUnloadKey(POBJECT_ATTRIBUTES targetKey);
NTSTATUS NTAPI NtFlushKey(HANDLE key);
}

#define SE_BACKUP_PRIVILEGE_ID   17
#define SE_RESTORE_PRIVILEGE_ID  18

#define DIRECTORY_QUERY 0x0001
#define SYMBOLIC_LINK_QUERY 0x0001

typedef struct _OBJECT_DIRECTORY_INFORMATION {
    UNICODE_STRING Name;
    UNICODE_STRING TypeName;
} OBJECT_DIRECTORY_INFORMATION;

namespace nt {

// Enable a privilege on the process token; logs and returns false on failure.
bool enablePrivilege(ULONG privilegeId, const wchar_t* nameForLog);

// Resolve a symbolic link in the NT object namespace (e.g. "\??\C:" -> "\Device\HarddiskVolume3").
bool querySymbolicLink(const std::wstring& linkPath, std::wstring* target, NTSTATUS* status = nullptr);

// List entries of an NT object directory ("\Device", "\ArcName", ...).
struct DirEntry { std::wstring name, type; };
bool listDirectory(const std::wstring& dirPath, std::vector<DirEntry>* out, NTSTATUS* status = nullptr);

// Load a registry hive file under HKLM\<keyName> (needs SeBackup+SeRestore).
NTSTATUS loadHive(const std::wstring& keyName, const std::wstring& ntFilePath);
NTSTATUS unloadHive(const std::wstring& keyName);

// "\Device\HarddiskVolume3" -> "\\?\GLOBALROOT\Device\HarddiskVolume3" (Win32-openable form).
std::wstring globalRoot(const std::wstring& ntPath);

}  // namespace nt
