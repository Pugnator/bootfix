#include "args.hh"
#include <algorithm>
#include <cwctype>
#include <stdlib.h>

const std::vector<OptionSpec>& optionTable()
{
    static const std::vector<OptionSpec> t = {
        // general
        { L"help",       L'h', false, nullptr,    L"show this help and exit",                                   L"*" },
        { L"version",    L'V', false, nullptr,    L"show the version and exit",                                 L"*" },
        { L"verbose",    L'v', false, nullptr,    L"full log lines on the console; twice for trace",            L"*" },
        { L"quiet",      L'q', false, nullptr,    L"only warnings and errors on the console",                   L"*" },
        { L"silent",     0,    false, nullptr,    L"same as --quiet",                                           L"*" },
        { L"debug",      0,    false, nullptr,    L"same as -vv",                                               L"*" },
        { L"log",        0,    true,  L"FILE",    L"log file (default: %LOCALAPPDATA%\\bootfix\\bootfix.log)",  L"*" },
        { L"no-color",   0,    false, nullptr,    L"no colour on the console",                                  L"*" },
        { L"no-input",   0,    false, nullptr,    L"never prompt; refuse instead",                              L"*" },
        { L"json",       0,    false, nullptr,    L"machine-readable output (scan, bcd dump)",                  L"*" },
        { L"gui",        0,    false, nullptr,    L"open the graphical interface instead",                      L"*" },
        // destructive-command behaviour
        { L"dry-run",    L'n', false, nullptr,    L"show what would be written, write nothing",                 L"fixmbr fixboot bcd" },
        { L"force",      L'f', false, nullptr,    L"do not ask for confirmation",                               L"fixmbr fixboot bcd" },
        { L"yes",        L'y', false, nullptr,    L"same as --force",                                           L"fixmbr fixboot bcd" },
        { L"backup-dir", 0,    true,  L"DIR",     L"where overwritten data is saved (default: see --log)",      L"fixmbr fixboot bcd" },
        { L"no-backup",  0,    false, nullptr,    L"do not save what is overwritten",                           L"fixmbr fixboot bcd" },
        // targets
        { L"disk",       L'd', true,  L"N",       L"physical disk number (see scan)",                           L"scan fixmbr" },
        { L"all",        L'a', false, nullptr,    L"every recognised volume on every disk",                     L"fixboot" },
        { L"nt52",       0,    false, nullptr,    L"NTLDR-compatible (XP) boot code instead of BOOTMGR",        L"fixmbr fixboot" },
        { L"dismount",   0,    false, nullptr,    L"write even if the volume cannot be locked",                 L"fixboot" },
        { L"from-backup",0,    false, nullptr,    L"restore sector 0 from the backup boot sector first",        L"fixboot" },
        // bcd
        { L"store",      L's', true,  L"FILE",    L"BCD file to work on",                                       L"bcd" },
        { L"live",       0,    false, nullptr,    L"this machine's mounted system store (HKLM\\BCD00000000)",   L"bcd" },
        { L"entry",      L'e', true,  L"ID",      L"object: {bootmgr}, {memdiag} or a {guid}",                  L"bcd" },
        { L"type",       L't', true,  L"TYPE",    L"object type: osloader, resume, bootmgr, memdiag, ...",      L"bcd" },
        { L"description",0,    true,  L"TEXT",    L"description for a new object",                              L"bcd" },
        { L"esp",        0,    true,  L"VOLUME",  L"system partition / ESP to put the boot files on",           L"bcd" },
        { L"firmware",   0,    true,  L"UEFI|BIOS", L"firmware of the target (default: from the disk)",       L"bcd" },
        { L"template",   0,    true,  L"FILE",    L"BCD-Template to start from (default: the Windows one)",     L"bcd" },
        { L"locale",     0,    true,  L"xx-XX",   L"locale for new entries (default: en-US)",                   L"bcd" },
        { L"recreate",   0,    false, nullptr,    L"start the store over from the template, do not merge",     L"bcd" },
    };
    return t;
}

namespace {

std::wstring lower(std::wstring s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return (wchar_t)towlower(c); });
    return s;
}

const OptionSpec* findLong(const std::wstring& name)
{
    for (const OptionSpec& o : optionTable())
        if (name == o.longName) return &o;
    return nullptr;
}

const OptionSpec* findShort(wchar_t c)
{
    for (const OptionSpec& o : optionTable())
        if (o.shortName && o.shortName == c) return &o;
    return nullptr;
}

size_t editDistance(const std::wstring& a, const std::wstring& b)
{
    std::vector<size_t> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); j++) prev[j] = j;
    for (size_t i = 1; i <= a.size(); i++) {
        cur[0] = i;
        for (size_t j = 1; j <= b.size(); j++)
            cur[j] = std::min(std::min(prev[j] + 1, cur[j - 1] + 1), prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1));
        prev.swap(cur);
    }
    return prev[b.size()];
}

}  // namespace

std::wstring suggest(const std::wstring& word, const std::vector<std::wstring>& candidates)
{
    std::wstring best;
    size_t bestD = 3;
    for (const std::wstring& c : candidates) {
        size_t d = editDistance(lower(word), lower(c));
        if (d < bestD) { bestD = d; best = c; }
    }
    return best;
}

bool Args::has(const wchar_t* name) const
{
    return options.find(name) != options.end();
}

std::wstring Args::get(const wchar_t* name, const wchar_t* def) const
{
    auto it = options.find(name);
    return it == options.end() ? std::wstring(def) : it->second;
}

bool Args::getInt(const wchar_t* name, long long* out, std::wstring* error) const
{
    auto it = options.find(name);
    if (it == options.end()) return false;
    wchar_t* end = nullptr;
    long long v = wcstoll(it->second.c_str(), &end, 0);
    if (!end || *end != 0 || it->second.empty()) {
        *error = std::wstring(L"--") + name + L" needs a number, got '" + it->second + L"'";
        return false;
    }
    *out = v;
    return true;
}

bool parseArgs(int argc, wchar_t** argv, Args* out, std::wstring* error)
{
    std::vector<std::wstring> words;
    bool optionsEnded = false;
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        if (optionsEnded || a == L"-" || a.empty() || a[0] != L'-') {
            words.push_back(a);
            continue;
        }
        if (a == L"--") { optionsEnded = true; continue; }
        if (a[1] == L'-') {
            std::wstring name = a.substr(2), value;
            size_t eq = name.find(L'=');
            bool hasValue = eq != std::wstring::npos;
            if (hasValue) { value = name.substr(eq + 1); name = name.substr(0, eq); }
            name = lower(name);
            const OptionSpec* spec = findLong(name);
            if (!spec) {
                std::vector<std::wstring> names;
                for (const OptionSpec& o : optionTable()) names.push_back(o.longName);
                std::wstring s = suggest(name, names);
                *error = L"unknown option --" + name + (s.empty() ? L"" : L" (did you mean --" + s + L"?)");
                return false;
            }
            if (spec->takesValue && !hasValue) {
                if (i + 1 >= argc) { *error = L"option --" + name + L" needs a value: --" + name + L" " + spec->valueName; return false; }
                value = argv[++i];
            } else if (!spec->takesValue && hasValue) {
                *error = L"option --" + name + L" does not take a value";
                return false;
            }
            if (name == L"verbose") out->verbosity++;
            else out->options[name] = value;
            continue;
        }
        // Short options, groupable: -nf, -vv, -d 2, -d2
        for (size_t k = 1; k < a.size(); k++) {
            const OptionSpec* spec = findShort(a[k]);
            if (!spec) { *error = std::wstring(L"unknown option -") + a[k]; return false; }
            if (spec->takesValue) {
                std::wstring value = a.substr(k + 1);
                if (value.empty()) {
                    if (i + 1 >= argc) { *error = std::wstring(L"option -") + a[k] + L" needs a value: -" + a[k] + L" " + spec->valueName; return false; }
                    value = argv[++i];
                }
                out->options[spec->longName] = value;
                break;
            }
            if (std::wstring(spec->longName) == L"verbose") out->verbosity++;
            else out->options[spec->longName] = L"";
        }
    }
    if (out->has(L"debug")) out->verbosity = std::max(out->verbosity, 2);
    if (out->has(L"silent")) out->options[L"quiet"] = L"";
    if (out->has(L"yes")) out->options[L"force"] = L"";

    size_t w = 0;
    if (w < words.size()) out->command = lower(words[w++]);
    if ((out->command == L"bcd" || out->command == L"help") && w < words.size())
        out->verb = lower(words[w++]);
    for (; w < words.size(); w++) out->operands.push_back(words[w]);
    return true;
}
