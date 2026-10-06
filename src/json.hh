// Minimal JSON writer for --json output (UI-CLI-008).  No parsing, no DOM:
// callers build the document in order.
#pragma once
#include <windows.h>
#include <string>
#include <vector>

class JsonWriter {
public:
    void beginObject() { sep(); buf += L'{'; stack.push_back(true); first = true; }
    void endObject()   { buf += L'}'; stack.pop_back(); first = false; }
    void beginArray()  { sep(); buf += L'['; stack.push_back(false); first = true; }
    void endArray()    { buf += L']'; stack.pop_back(); first = false; }
    void key(const wchar_t* k) { sep(); buf += quote(k); buf += L':'; pendingKey = true; }
    void value(const std::wstring& s) { sep(); buf += quote(s); }
    void value(const wchar_t* s) { value(std::wstring(s)); }
    void value(long long v) { sep(); buf += std::to_wstring(v); }
    void value(unsigned long long v) { sep(); buf += std::to_wstring(v); }
    void value(int v) { value((long long)v); }
    void value(unsigned long v) { value((unsigned long long)v); }
    void value(bool b) { sep(); buf += b ? L"true" : L"false"; }
    void null() { sep(); buf += L"null"; }
    void kv(const wchar_t* k, const std::wstring& v) { key(k); value(v); }
    void kv(const wchar_t* k, const wchar_t* v) { key(k); value(v); }
    void kv(const wchar_t* k, long long v) { key(k); value(v); }
    void kv(const wchar_t* k, unsigned long long v) { key(k); value(v); }
    void kv(const wchar_t* k, int v) { key(k); value(v); }
    void kv(const wchar_t* k, unsigned long v) { key(k); value(v); }
    void kv(const wchar_t* k, bool v) { key(k); value(v); }
    const std::wstring& str() const { return buf; }

private:
    void sep()
    {
        if (pendingKey) { pendingKey = false; return; }
        if (!first) buf += L',';
        first = false;
    }
    static std::wstring quote(const std::wstring& s)
    {
        std::wstring o = L"\"";
        for (wchar_t c : s) {
            switch (c) {
            case L'"': o += L"\\\""; break;
            case L'\\': o += L"\\\\"; break;
            case L'\n': o += L"\\n"; break;
            case L'\r': o += L"\\r"; break;
            case L'\t': o += L"\\t"; break;
            default:
                if (c < 0x20) { wchar_t b[8]; _snwprintf_s(b, 8, _TRUNCATE, L"\\u%04x", c); o += b; }
                else o += c;
            }
        }
        return o + L"\"";
    }
    std::wstring buf;
    std::vector<bool> stack;
    bool first = true, pendingKey = false;
};
