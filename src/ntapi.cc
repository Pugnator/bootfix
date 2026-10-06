#include "ntapi.hh"
#include "log.hh"

namespace nt {

bool enablePrivilege(ULONG privilegeId, const wchar_t* nameForLog)
{
    BOOLEAN was = FALSE;
    NTSTATUS st = RtlAdjustPrivilege(privilegeId, TRUE, FALSE, &was);
    if (!NT_SUCCESS(st)) {
        LOG_ERROR(L"cannot enable %s: 0x%08X (%s) - run elevated", nameForLog, (unsigned)st,
                  logging::ntStatus(st).c_str());
        return false;
    }
    LOG_TRACE(L"%s enabled (was %s)", nameForLog, was ? L"on" : L"off");
    return true;
}

static void initOa(OBJECT_ATTRIBUTES* oa, UNICODE_STRING* us, const std::wstring& path)
{
    us->Buffer = const_cast<wchar_t*>(path.c_str());
    us->Length = (USHORT)(path.size() * sizeof(wchar_t));
    us->MaximumLength = us->Length + sizeof(wchar_t);
    InitializeObjectAttributes(oa, us, OBJ_CASE_INSENSITIVE, NULL, NULL);
}

bool querySymbolicLink(const std::wstring& linkPath, std::wstring* target, NTSTATUS* status)
{
    UNICODE_STRING us;
    OBJECT_ATTRIBUTES oa;
    initOa(&oa, &us, linkPath);
    HANDLE h;
    NTSTATUS st = NtOpenSymbolicLinkObject(&h, SYMBOLIC_LINK_QUERY, &oa);
    if (status) *status = st;
    if (!NT_SUCCESS(st)) {
        LOG_TRACE(L"NtOpenSymbolicLinkObject(%s): 0x%08X", linkPath.c_str(), (unsigned)st);
        return false;
    }
    wchar_t buf[1024];
    UNICODE_STRING out = { 0, sizeof buf, buf };
    ULONG len = 0;
    st = NtQuerySymbolicLinkObject(h, &out, &len);
    NtClose(h);
    if (status) *status = st;
    if (!NT_SUCCESS(st)) {
        LOG_TRACE(L"NtQuerySymbolicLinkObject(%s): 0x%08X", linkPath.c_str(), (unsigned)st);
        return false;
    }
    target->assign(out.Buffer, out.Length / sizeof(wchar_t));
    LOG_TRACE(L"%s -> %s", linkPath.c_str(), target->c_str());
    return true;
}

bool listDirectory(const std::wstring& dirPath, std::vector<DirEntry>* out, NTSTATUS* status)
{
    UNICODE_STRING us;
    OBJECT_ATTRIBUTES oa;
    initOa(&oa, &us, dirPath);
    HANDLE h;
    NTSTATUS st = NtOpenDirectoryObject(&h, DIRECTORY_QUERY, &oa);
    if (status) *status = st;
    if (!NT_SUCCESS(st)) {
        LOG_DEBUG(L"NtOpenDirectoryObject(%s): 0x%08X (%s)", dirPath.c_str(), (unsigned)st,
                  logging::ntStatus(st).c_str());
        return false;
    }
    std::vector<BYTE> buf(64 * 1024);
    ULONG ctx = 0, ret = 0;
    BOOLEAN restart = TRUE;
    for (;;) {
        st = NtQueryDirectoryObject(h, buf.data(), (ULONG)buf.size(), FALSE, restart, &ctx, &ret);
        restart = FALSE;
        if (!NT_SUCCESS(st))
            break;
        auto* e = reinterpret_cast<OBJECT_DIRECTORY_INFORMATION*>(buf.data());
        for (; e->Name.Buffer; e++) {
            DirEntry d;
            d.name.assign(e->Name.Buffer, e->Name.Length / sizeof(wchar_t));
            d.type.assign(e->TypeName.Buffer, e->TypeName.Length / sizeof(wchar_t));
            out->push_back(d);
        }
        if (st != STATUS_MORE_ENTRIES)
            break;
    }
    NtClose(h);
    if (status) *status = st;
    if (st == STATUS_NO_MORE_ENTRIES)
        st = STATUS_SUCCESS;
    return NT_SUCCESS(st);
}

NTSTATUS loadHive(const std::wstring& keyName, const std::wstring& ntFilePath)
{
    std::wstring key = L"\\Registry\\Machine\\" + keyName;
    UNICODE_STRING usKey, usFile;
    OBJECT_ATTRIBUTES oaKey, oaFile;
    initOa(&oaKey, &usKey, key);
    initOa(&oaFile, &usFile, ntFilePath);
    NTSTATUS st = NtLoadKey(&oaKey, &oaFile);
    if (NT_SUCCESS(st))
        LOG_DEBUG(L"hive %s loaded at HKLM\\%s", ntFilePath.c_str(), keyName.c_str());
    else
        LOG_ERROR(L"NtLoadKey(%s <- %s): 0x%08X (%s)", keyName.c_str(), ntFilePath.c_str(),
                  (unsigned)st, logging::ntStatus(st).c_str());
    return st;
}

NTSTATUS unloadHive(const std::wstring& keyName)
{
    std::wstring key = L"\\Registry\\Machine\\" + keyName;
    UNICODE_STRING usKey;
    OBJECT_ATTRIBUTES oaKey;
    initOa(&oaKey, &usKey, key);
    NTSTATUS st = NtUnloadKey(&oaKey);
    if (!NT_SUCCESS(st))
        LOG_WARN(L"NtUnloadKey(%s): 0x%08X (%s)", keyName.c_str(), (unsigned)st,
                 logging::ntStatus(st).c_str());
    return st;
}

std::wstring globalRoot(const std::wstring& ntPath)
{
    return L"\\\\?\\GLOBALROOT" + ntPath;
}

}  // namespace nt
