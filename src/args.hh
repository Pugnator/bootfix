// Argument parsing after POSIX/GNU conventions (UI-CLI-005):
//   bootfix <command> [<verb>] [options] [operands]
// Options: --name value, --name=value, --flag, -x, -xyz grouped, "--" ends options.
// Every option is declared in the table below; anything else is an error with a
// suggestion (UI-CLI-007).  The same table generates the help text.
#pragma once
#include <map>
#include <string>
#include <vector>

struct OptionSpec {
    const wchar_t* longName;   // without the leading --
    wchar_t        shortName;  // 0 when none
    bool           takesValue;
    const wchar_t* valueName;  // shown in help, e.g. "N"
    const wchar_t* help;       // lower-case, no trailing period (UI-CLI-003)
    const wchar_t* commands;   // space-separated command names it applies to, "*" for all
};

const std::vector<OptionSpec>& optionTable();

struct Args {
    std::wstring command;                  // "scan", "fixmbr", "fixboot", "bcd", "help", "version"
    std::wstring verb;                     // for "bcd": "dump", "set", ...; for "help": the command
    std::vector<std::wstring> operands;    // remaining non-option words
    std::map<std::wstring, std::wstring> options;  // long name -> value ("" for flags)
    int verbosity = 0;                     // -v count

    bool has(const wchar_t* name) const;
    std::wstring get(const wchar_t* name, const wchar_t* def = L"") const;
    // Returns false when absent; sets *error and returns false on a non-number.
    bool getInt(const wchar_t* name, long long* out, std::wstring* error) const;
};

// Returns false and fills `error` (already in "message" form, no prefix) on malformed input.
bool parseArgs(int argc, wchar_t** argv, Args* out, std::wstring* error);

// Closest known word for a typo (edit distance <= 2), or "".
std::wstring suggest(const std::wstring& word, const std::vector<std::wstring>& candidates);
