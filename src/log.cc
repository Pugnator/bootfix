#include "log.hh"
#include <stdio.h>
#include <vector>

namespace {

HANDLE           g_file = INVALID_HANDLE_VALUE;
LogLevel         g_consoleLevel = LOG_LEVEL_INFO;
bool             g_verboseConsole = false;
bool             g_color = false;
CRITICAL_SECTION g_lock;
bool             g_lockInit = false;
logging::Sink    g_sink;

const wchar_t* levelName(LogLevel l)
{
    switch (l) {
    case LOG_LEVEL_ERROR: return L"ERROR";
    case LOG_LEVEL_WARN:  return L"WARN ";
    case LOG_LEVEL_INFO:  return L"INFO ";
    case LOG_LEVEL_DEBUG: return L"DEBUG";
    default:              return L"TRACE";
    }
}

void writeUtf8(HANDLE h, const wchar_t* text, size_t len)
{
    if (!len) return;
    std::vector<char> buf(len * 3 + 1);
    int n = WideCharToMultiByte(CP_UTF8, 0, text, (int)len, buf.data(), (int)buf.size(), NULL, NULL);
    DWORD w;
    if (n > 0) WriteFile(h, buf.data(), (DWORD)n, &w, NULL);
}

// Console if it is one, UTF-8 bytes otherwise (UI-CLI-023).  Colour only via console
// attributes, so no escape sequence can ever reach a pipe or the log (UI-CLI-009).
void writeStream(DWORD stdHandle, const wchar_t* text, size_t len, WORD attr)
{
    HANDLE h = GetStdHandle(stdHandle);
    if (h == INVALID_HANDLE_VALUE || h == NULL) return;
    DWORD mode, w;
    if (GetConsoleMode(h, &mode)) {
        CONSOLE_SCREEN_BUFFER_INFO info = {};
        bool colored = g_color && attr && GetConsoleScreenBufferInfo(h, &info);
        if (colored) SetConsoleTextAttribute(h, attr);
        WriteConsoleW(h, text, (DWORD)len, &w, NULL);
        if (colored) SetConsoleTextAttribute(h, info.wAttributes);
    } else {
        writeUtf8(h, text, len);
    }
}

void toFile(const std::wstring& line)
{
    if (g_file != INVALID_HANDLE_VALUE) writeUtf8(g_file, line.c_str(), line.size());
}

std::wstring timestamp()
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[40];
    _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%04u-%02u-%02u %02u:%02u:%02u.%03u",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return buf;
}

std::wstring trimMessage(wchar_t* buf, DWORD n)
{
    while (n && (buf[n - 1] == L'\r' || buf[n - 1] == L'\n' || buf[n - 1] == L' ' || buf[n - 1] == L'.'))
        buf[--n] = 0;
    return std::wstring(buf, n);
}

}  // namespace

namespace logging {

DWORD init(const wchar_t* filePath, LogLevel consoleLevel)
{
    if (!g_lockInit) {
        InitializeCriticalSection(&g_lock);
        g_lockInit = true;
    }
    g_consoleLevel = consoleLevel;
    if (filePath && *filePath) {
        g_file = CreateFileW(filePath, FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (g_file == INVALID_HANDLE_VALUE)
            return GetLastError();
        LARGE_INTEGER size;
        if (GetFileSizeEx(g_file, &size) && size.QuadPart == 0) {
            DWORD w;
            WriteFile(g_file, "\xEF\xBB\xBF", 3, &w, NULL);  // UTF-8 BOM on a fresh file
        }
    }
    return 0;
}

void shutdown()
{
    if (g_file != INVALID_HANDLE_VALUE) {
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
}

void setLevel(LogLevel level) { g_consoleLevel = level; }
LogLevel level() { return g_consoleLevel; }
void setVerboseConsole(bool on) { g_verboseConsole = on; }
void setColor(bool on) { g_color = on; }
void setSink(Sink sink) { g_sink = sink; }

bool consoleIsTerminal(DWORD stdHandle)
{
    HANDLE h = GetStdHandle(stdHandle);
    DWORD mode;
    return h != INVALID_HANDLE_VALUE && h != NULL && GetConsoleMode(h, &mode) != 0;
}

void writev(LogLevel level, const char* func, const wchar_t* fmt, va_list ap)
{
    wchar_t msg[4096];
    _vsnwprintf_s(msg, _countof(msg), _TRUNCATE, fmt, ap);

    wchar_t full[4096 + 128];
    _snwprintf_s(full, _countof(full), _TRUNCATE, L"%s %s [%S] %s\r\n",
                 timestamp().c_str(), levelName(level), func ? func : "?", msg);

    if (g_lockInit) EnterCriticalSection(&g_lock);
    toFile(full);
    if (level <= g_consoleLevel) {
        if (g_verboseConsole) {
            writeStream(STD_ERROR_HANDLE, full, wcslen(full), level == LOG_LEVEL_ERROR ? FOREGROUND_RED | FOREGROUND_INTENSITY
                                                                 : level == LOG_LEVEL_WARN ? FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY : 0);
        } else {
            // GNU form: "program: message"; warnings marked; progress lines bare (UI-CLI-011).
            std::wstring line;
            WORD attr = 0;
            if (level == LOG_LEVEL_ERROR) { line = L"bootfix: "; attr = FOREGROUND_RED | FOREGROUND_INTENSITY; }
            else if (level == LOG_LEVEL_WARN) { line = L"bootfix: warning: "; attr = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY; }
            line += msg;
            line += L"\r\n";
            writeStream(STD_ERROR_HANDLE, line.c_str(), line.size(), attr);
        }
    }
    if (g_sink) g_sink((int)level, msg);
    if (g_lockInit) LeaveCriticalSection(&g_lock);
}

void write(LogLevel level, const char* func, const wchar_t* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    writev(level, func, fmt, ap);
    va_end(ap);
}

void outLine(const std::wstring& line)
{
    if (g_lockInit) EnterCriticalSection(&g_lock);
    std::wstring crlf = line + L"\r\n";
    writeStream(STD_OUTPUT_HANDLE, crlf.c_str(), crlf.size(), 0);
    toFile(timestamp() + L" OUT   " + crlf);
    if (g_sink) g_sink(-1, line);
    if (g_lockInit) LeaveCriticalSection(&g_lock);
}

void out(const wchar_t* fmt, ...)
{
    wchar_t msg[8192];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(msg, _countof(msg), _TRUNCATE, fmt, ap);
    va_end(ap);
    outLine(msg);
}

void hexdump(LogLevel level, const char* func, const wchar_t* title, const void* data, size_t len)
{
    if (level > g_consoleLevel && g_file == INVALID_HANDLE_VALUE)
        return;
    const unsigned char* p = static_cast<const unsigned char*>(data);
    write(level, func, L"%s (%Iu bytes)", title, len);
    for (size_t off = 0; off < len; off += 16) {
        wchar_t hex[16 * 3 + 1] = {0}, asc[17] = {0};
        for (size_t i = 0; i < 16 && off + i < len; i++) {
            unsigned char c = p[off + i];
            _snwprintf_s(hex + i * 3, 4, _TRUNCATE, L"%02X ", c);
            asc[i] = (c >= 0x20 && c < 0x7f) ? (wchar_t)c : L'.';
        }
        write(level, func, L"  %04Ix: %-48s |%s|", off, hex, asc);
    }
}

std::wstring win32Error(DWORD err)
{
    wchar_t buf[512];
    DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                             NULL, err, 0, buf, _countof(buf), NULL);
    if (n == 0) {
        _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"Windows error %lu", err);
        return buf;
    }
    return trimMessage(buf, n);
}

std::wstring ntStatus(LONG status)
{
    wchar_t buf[512];
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS,
                             ntdll, (DWORD)status, 0, buf, _countof(buf), NULL);
    if (n == 0) {
        _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"NTSTATUS 0x%08X", (unsigned)status);
        return buf;
    }
    return trimMessage(buf, n);
}

}  // namespace logging
