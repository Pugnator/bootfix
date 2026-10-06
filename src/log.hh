// Logging and output.
//
// Two streams, as the CLI rules require (UI-CLI-002):
//   * the product goes to stdout through logging::out()
//   * messages (errors, warnings, progress) go to stderr through LOG_*()
// The log file receives everything at TRACE with timestamps; the console
// receives messages in the GNU form "bootfix: message" unless --verbose asks
// for the full line (UI-CLI-011).  A GUI can attach a sink to receive both.
#pragma once
#include <windows.h>
#include <stdarg.h>
#include <functional>
#include <string>

enum LogLevel {
    LOG_LEVEL_ERROR = 0,
    LOG_LEVEL_WARN  = 1,
    LOG_LEVEL_INFO  = 2,
    LOG_LEVEL_DEBUG = 3,
    LOG_LEVEL_TRACE = 4,
};

namespace logging {

// Returns 0 on success, Win32 error if the log file cannot be opened.
DWORD init(const wchar_t* filePath, LogLevel consoleLevel);
void shutdown();
void setLevel(LogLevel level);
LogLevel level();
// Full lines (timestamp, level, function) on the console instead of "bootfix: message".
void setVerboseConsole(bool on);
// Colour on the console: decided from TTY / NO_COLOR / TERM=dumb / --no-color (UI-CLI-009).
void setColor(bool on);
bool consoleIsTerminal(DWORD stdHandle);

// Message sink for a GUI: receives every message (any level) and every product line (level -1).
typedef std::function<void(int level, const std::wstring& line)> Sink;
void setSink(Sink sink);

void write(LogLevel level, const char* func, const wchar_t* fmt, ...);
void writev(LogLevel level, const char* func, const wchar_t* fmt, va_list ap);

// The product: one line to stdout (and to the log file as a TRACE "out:" line).
void out(const wchar_t* fmt, ...);
void outLine(const std::wstring& line);

// Hex dump (16 bytes per line).
void hexdump(LogLevel level, const char* func, const wchar_t* title, const void* data, size_t len);

std::wstring win32Error(DWORD err);
std::wstring ntStatus(LONG status);

}  // namespace logging

#define LOG_ERROR(...) ::logging::write(LOG_LEVEL_ERROR, __func__, __VA_ARGS__)
#define LOG_WARN(...)  ::logging::write(LOG_LEVEL_WARN,  __func__, __VA_ARGS__)
#define LOG_INFO(...)  ::logging::write(LOG_LEVEL_INFO,  __func__, __VA_ARGS__)
#define LOG_DEBUG(...) ::logging::write(LOG_LEVEL_DEBUG, __func__, __VA_ARGS__)
#define LOG_TRACE(...) ::logging::write(LOG_LEVEL_TRACE, __func__, __VA_ARGS__)

// Log a failed Win32 call with GetLastError() decoded.
#define LOG_LASTERR(what) do { DWORD e_ = ::GetLastError(); \
    ::logging::write(LOG_LEVEL_ERROR, __func__, L"%s: %s (error %lu)", (what), ::logging::win32Error(e_).c_str(), e_); } while (0)
#define LOG_NT(what, st) \
    ::logging::write(LOG_LEVEL_ERROR, __func__, L"%s: %s (status 0x%08X)", (what), ::logging::ntStatus(st).c_str(), (unsigned)(st))
