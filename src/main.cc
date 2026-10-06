// bootfix - re-implementation of the Windows boot repair utilities
// (bootrec / bootsect / bcdboot / bcdedit) with logging, conventional
// arguments and an optional graphical interface.
#include "args.hh"
#include "commands.hh"
#include "log.hh"
#include "version.hh"
#include <io.h>
#include <shlobj.h>
#include <stdio.h>
#include <string>
#include <vector>

#ifdef BOOTFIX_GUI
#include "gui.hh"
#endif

namespace {

const wchar_t* const kCommands[] = { L"scan", L"fixmbr", L"fixboot", L"bcd", L"help", L"version" };
const wchar_t* const kBcdVerbs[] = { L"dump", L"rebuild", L"set", L"unset", L"create", L"delete", L"export" };

volatile LONG g_interrupted = 0;

BOOL WINAPI ctrlHandler(DWORD type)
{
    if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT) return FALSE;
    InterlockedExchange(&g_interrupted, 1);
    // Say so at once (UI-CLI-017); sector writes are single calls, nothing to clean up.
    const char msg[] = "bootfix: interrupted\r\n";
    DWORD w;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), msg, sizeof msg - 1, &w, NULL);
    ExitProcess(130);
}

std::wstring localAppData()
{
    wchar_t* p = nullptr;
    std::wstring r;
    if (SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, NULL, &p) == S_OK) { r = p; CoTaskMemFree(p); }
    if (r.empty()) {
        wchar_t tmp[MAX_PATH];
        if (GetTempPathW(MAX_PATH, tmp)) r = tmp;
    }
    return r;
}

std::wstring appDir()
{
    std::wstring d = localAppData() + L"\\bootfix";
    CreateDirectoryW(d.c_str(), NULL);
    return d;
}

void putLine(const wchar_t* s) { logging::outLine(s); }

void printGlobalOptions()
{
    putLine(L"");
    putLine(L"Options for every command:");
    for (const OptionSpec& o : optionTable()) {
        if (wcscmp(o.commands, L"*") != 0) continue;
        wchar_t line[200], flag[64];
        if (o.shortName) _snwprintf_s(flag, _countof(flag), _TRUNCATE, L"-%c, --%s", o.shortName, o.longName);
        else _snwprintf_s(flag, _countof(flag), _TRUNCATE, L"    --%s", o.longName);
        if (o.takesValue) { wcscat_s(flag, L" "); wcscat_s(flag, o.valueName); }
        _snwprintf_s(line, _countof(line), _TRUNCATE, L"  %-22s %s", flag, o.help);
        putLine(line);
    }
}

void printCommandOptions(const wchar_t* cmd)
{
    bool any = false;
    for (const OptionSpec& o : optionTable()) {
        if (wcscmp(o.commands, L"*") == 0) continue;
        std::wstring list = std::wstring(L" ") + o.commands + L" ";
        if (list.find(std::wstring(L" ") + cmd + L" ") == std::wstring::npos) continue;
        if (!any) { putLine(L""); putLine(L"Options:"); any = true; }
        wchar_t line[200], flag[64];
        if (o.shortName) _snwprintf_s(flag, _countof(flag), _TRUNCATE, L"-%c, --%s", o.shortName, o.longName);
        else _snwprintf_s(flag, _countof(flag), _TRUNCATE, L"    --%s", o.longName);
        if (o.takesValue) { wcscat_s(flag, L" "); wcscat_s(flag, o.valueName); }
        _snwprintf_s(line, _countof(line), _TRUNCATE, L"  %-22s %s", flag, o.help);
        putLine(line);
    }
}

void printFooter()
{
    putLine(L"");
    putLine(L"Home page and bug reports: " BOOTFIX_HOMEPAGE);
    putLine(L"Every run is logged in full to %LOCALAPPDATA%\\bootfix\\bootfix.log");
}

void printHelp(const std::wstring& cmd, const std::wstring& verb)
{
    if (cmd.empty() || cmd == L"help") {
        putLine(L"bootfix " BOOTFIX_VERSION L" - repair Windows boot: MBR and volume boot code, BCD store");
        putLine(L"");
        putLine(L"Usage: bootfix <command> [options]");
        putLine(L"       bootfix bcd <verb> [options]");
        putLine(L"       bootfix --gui");
        putLine(L"");
        putLine(L"Commands:");
        putLine(L"  scan         report disks, partitions, boot code, boot files, Windows installs");
        putLine(L"  fixmbr       write Windows MBR boot code to a disk, keep its partition table");
        putLine(L"  fixboot      write Windows boot code to a volume, keeping its BPB");
        putLine(L"  bcd dump     list every object and element of a BCD store");
        putLine(L"  bcd rebuild  copy boot files to the system partition, add missing boot entries");
        putLine(L"  bcd set      set elements of an object (bcdedit /set)");
        putLine(L"  bcd unset    remove elements of an object (bcdedit /deletevalue)");
        putLine(L"  bcd create   create an object (bcdedit /create)");
        putLine(L"  bcd delete   delete an object (bcdedit /delete)");
        putLine(L"  bcd export   save a store to a file (bcdedit /export)");
        putLine(L"  help         show help for a command: bootfix help fixboot");
        putLine(L"  version      show the version");
        putLine(L"");
        putLine(L"Examples:");
        putLine(L"  bootfix scan                      what is on the attached disks");
        putLine(L"  bootfix fixmbr --disk 2 -n        show what fixmbr would write");
        putLine(L"  bootfix fixboot F:                BOOTMGR boot code for volume F:");
        putLine(L"  bootfix bcd rebuild F:\\Windows --esp S:");
        putLine(L"  bootfix bcd dump --live --json");
        printGlobalOptions();
        printFooter();
        return;
    }
    if (cmd == L"scan") {
        putLine(L"Usage: bootfix scan [--disk N] [--json]");
        putLine(L"");
        putLine(L"Reads every disk: partition table, MBR boot code, each volume's boot sector");
        putLine(L"and its backup copy, boot files (bootmgr, BCD, bootmgfw.efi) and Windows");
        putLine(L"installations. Writes nothing. One record per line; --json for structure.");
        putLine(L"");
        putLine(L"Examples:");
        putLine(L"  bootfix scan");
        putLine(L"  bootfix scan --disk 1 --json");
    } else if (cmd == L"fixmbr") {
        putLine(L"Usage: bootfix fixmbr --disk N [--nt52] [-n] [-f]");
        putLine(L"");
        putLine(L"Copies the 440 bytes of Windows MBR boot code (bootrec /FixMbr, bootsect /mbr)");
        putLine(L"into sector 0 of the disk. The disk signature and partition table are kept.");
        putLine(L"The old sector is saved in the backup directory first.");
        putLine(L"");
        putLine(L"Examples:");
        putLine(L"  bootfix fixmbr --disk 2 --dry-run");
        putLine(L"  bootfix fixmbr --disk 2 --force");
    } else if (cmd == L"fixboot") {
        putLine(L"Usage: bootfix fixboot VOLUME... [--nt52] [--from-backup] [--dismount] [-n] [-f]");
        putLine(L"       bootfix fixboot --all");
        putLine(L"");
        putLine(L"Writes the Windows boot code for the volume's file system (FAT, FAT32, NTFS,");
        putLine(L"exFAT) like bootsect /nt60, keeping the BPB. When sector 0 is unreadable, or");
        putLine(L"with --from-backup, the backup boot sector is restored first like");
        putLine(L"bootrec /FixBoot. VOLUME is a drive letter, \\Device\\HarddiskVolumeN or");
        putLine(L"\\\\?\\Volume{...}. The old sectors are saved in the backup directory first.");
        putLine(L"");
        putLine(L"Examples:");
        putLine(L"  bootfix fixboot F: --dry-run");
        putLine(L"  bootfix fixboot \\Device\\HarddiskVolume5 --from-backup");
        putLine(L"  bootfix fixboot --all --nt52");
    } else if (cmd == L"bcd") {
        if (verb.empty() || verb == L"dump") {
            putLine(L"Usage: bootfix bcd dump [--store FILE | --live] [--json]");
            putLine(L"");
            putLine(L"Lists every object and element of a BCD store with devices decoded.");
            putLine(L"Without --store or --live, every BCD file on the attached disks is listed.");
        }
        if (verb.empty() || verb == L"rebuild") {
            putLine(L"Usage: bootfix bcd rebuild [WINDIR...] [--esp VOLUME] [--firmware UEFI|BIOS]");
            putLine(L"                   [--recreate] [--template FILE] [--locale xx-XX] [-n] [-f]");
            putLine(L"");
            putLine(L"Copies the boot files from the first Windows installation to the system");
            putLine(L"partition (bcdboot), then adds a boot entry for every Windows installation");
            putLine(L"that the store does not have yet (bootrec /RebuildBcd). --recreate starts");
            putLine(L"the store over from BCD-Template. WINDIR limits the installations, e.g.");
            putLine(L"F:\\Windows; without it every installation scan can see is used.");
        }
        if (verb.empty() || verb == L"set") {
            putLine(L"Usage: bootfix bcd set --entry ID NAME=VALUE... (--store FILE | --live)");
            putLine(L"");
            putLine(L"Sets elements; NAME is a bcdedit name (timeout, osdevice, displayorder) or");
            putLine(L"custom:XXXXXXXX. Devices: partition=X: or boot. Lists: a,b,c.");
        }
        if (verb.empty() || verb == L"unset")
            putLine(L"Usage: bootfix bcd unset --entry ID NAME... (--store FILE | --live)");
        if (verb.empty() || verb == L"create")
            putLine(L"Usage: bootfix bcd create --type TYPE [--entry ID] [--description TEXT]");
        if (verb.empty() || verb == L"delete")
            putLine(L"Usage: bootfix bcd delete --entry ID (--store FILE | --live)");
        if (verb.empty() || verb == L"export")
            putLine(L"Usage: bootfix bcd export FILE (--store FILE | --live)");
        putLine(L"");
        putLine(L"Examples:");
        putLine(L"  bootfix bcd dump --live");
        putLine(L"  bootfix bcd set --entry {bootmgr} timeout=5 --live");
        putLine(L"  bootfix bcd set --entry {bootmgr} displayorder={a},{b} --store S:\\Boot\\BCD");
        putLine(L"  bootfix bcd rebuild F:\\Windows --esp S: --firmware UEFI --dry-run");
    } else if (cmd == L"version") {
        putLine(L"Usage: bootfix version");
    }
    printCommandOptions(cmd.c_str());
    printGlobalOptions();
    printFooter();
}

void printVersion()
{
    putLine(L"bootfix " BOOTFIX_VERSION);
    putLine(L"Boot code and BCD logic reconstructed from Windows 10 build 17763 bootrec,");
    putLine(L"bootsect, bcdboot and bcdedit. No warranty.");
}

std::wstring findDefaultLog()
{
    return appDir() + L"\\bootfix.log";
}

}  // namespace

bool interrupted() { return g_interrupted != 0; }

bool isElevated()
{
    HANDLE tok;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION te = {};
    DWORD len = sizeof te;
    bool ok = GetTokenInformation(tok, TokenElevation, &te, sizeof te, &len) && te.TokenIsElevated;
    CloseHandle(tok);
    return ok;
}

bool requireElevation(const wchar_t* what)
{
    if (isElevated()) return true;
    LOG_ERROR(L"%s needs an administrator prompt: Windows refuses raw disk access and BCD hive loading otherwise", what);
    return false;
}

std::wstring backupDir(const Args& args)
{
    std::wstring d = args.get(L"backup-dir");
    if (d.empty()) d = appDir() + L"\\backups";
    CreateDirectoryW(d.c_str(), NULL);
    return d;
}

int confirmOrDryRun(const Args& args, const wchar_t* question)
{
    if (args.has(L"dry-run")) {
        LOG_INFO(L"dry run: %s - nothing written", question);
        return EXIT_OK;
    }
    if (args.has(L"force"))
        return -1;
    bool tty = _isatty(_fileno(stdin)) != 0;
    if (!tty || args.has(L"no-input")) {
        LOG_ERROR(L"%s needs confirmation and there is no terminal to ask; pass --force to proceed", question);
        return EXIT_FAILED;
    }
    fwprintf(stderr, L"%s [y/N] ", question);
    fflush(stderr);
    wchar_t buf[16] = {};
    if (!fgetws(buf, _countof(buf), stdin)) return EXIT_OK;
    if (buf[0] == L'y' || buf[0] == L'Y') return -1;
    LOG_INFO(L"not confirmed; nothing written");
    return EXIT_OK;
}

int wmain(int argc, wchar_t** argv)
{
    SetConsoleCtrlHandler(ctrlHandler, TRUE);

    Args args;
    std::wstring err;
    if (!parseArgs(argc, argv, &args, &err)) {
        logging::setColor(false);
        LOG_ERROR(L"%s", err.c_str());
        LOG_ERROR(L"run 'bootfix help' for the options");
        return EXIT_USAGE;
    }

    // --help / -h anywhere wins; so does --version (UI-CLI-003, UI-CLI-004).
    if (args.has(L"version") || args.command == L"version") {
        if (!args.has(L"help")) { printVersion(); return EXIT_OK; }
    }
    if (args.has(L"help") || args.command == L"help" || (args.command.empty() && !args.has(L"gui"))) {
        std::wstring c = args.command == L"help" ? args.verb : args.command;
        std::wstring v = args.command == L"help" ? (args.operands.empty() ? L"" : args.operands[0]) : args.verb;
        bool known = false;
        for (auto k : kCommands) known = known || c == k;
        if (!known && args.command == L"help" && !c.empty()) {
            LOG_ERROR(L"unknown command '%s'; run 'bootfix help' for the list", c.c_str());
            return EXIT_USAGE;
        }
        // -h / --help wins over everything else on the line (UI-CLI-003).
        printHelp(known ? c : L"", known ? v : L"");
        return EXIT_OK;
    }
    // Console presentation: level, colour, verbosity (UI-CLI-009, UI-CLI-011).
    LogLevel level = args.verbosity >= 2 ? LOG_LEVEL_TRACE : args.verbosity == 1 ? LOG_LEVEL_DEBUG : LOG_LEVEL_INFO;
    if (args.has(L"quiet")) level = LOG_LEVEL_WARN;
    logging::setVerboseConsole(args.verbosity >= 1);
    wchar_t envbuf[16];
    bool noColorEnv = GetEnvironmentVariableW(L"NO_COLOR", envbuf, _countof(envbuf)) > 0;
    bool dumbTerm = GetEnvironmentVariableW(L"TERM", envbuf, _countof(envbuf)) > 0 && wcscmp(envbuf, L"dumb") == 0;
    logging::setColor(!args.has(L"no-color") && !noColorEnv && !dumbTerm && logging::consoleIsTerminal(STD_ERROR_HANDLE));

    std::wstring logPath = args.get(L"log");
    if (logPath.empty()) logPath = findDefaultLog();
    DWORD le = logging::init(logPath.c_str(), level);
    if (le != 0)
        LOG_WARN(L"cannot open the log file %s: %s; continuing without it", logPath.c_str(), logging::win32Error(le).c_str());

    std::wstring cmdline;
    for (int i = 0; i < argc; i++) { if (i) cmdline += L' '; cmdline += argv[i]; }
    LOG_DEBUG(L"==== bootfix " BOOTFIX_VERSION L" start: %s", cmdline.c_str());

    int rc;
    if (args.has(L"gui")) {
#ifdef BOOTFIX_GUI
        rc = runGui(args);
#else
        LOG_ERROR(L"this build has no graphical interface; build with the imgui submodule (git submodule update --init) and BOOTFIX_GUI=ON");
        rc = EXIT_USAGE;
#endif
    } else {
        bool known = false;
        for (auto k : kCommands) known = known || args.command == k;
        if (!known) {
            std::vector<std::wstring> names(kCommands, kCommands + _countof(kCommands));
            std::wstring s = suggest(args.command, names);
            LOG_ERROR(L"unknown command '%s'%s; run 'bootfix help' for the list", args.command.c_str(),
                      s.empty() ? L"" : (L" (did you mean '" + s + L"'?)").c_str());
            rc = EXIT_USAGE;
        } else if (args.command == L"bcd" && args.verb.empty()) {
            LOG_ERROR(L"bcd needs a verb: dump, rebuild, set, unset, create, delete or export");
            rc = EXIT_USAGE;
        } else {
            try {
                if (args.command == L"scan")         rc = cmdScan(args);
                else if (args.command == L"fixmbr")  rc = cmdFixMbr(args);
                else if (args.command == L"fixboot") rc = cmdFixBoot(args);
                else                                 rc = cmdBcd(args);
            } catch (const std::wstring& e) {
                LOG_ERROR(L"%s", e.c_str());
                rc = EXIT_USAGE;
            }
        }
    }
    LOG_DEBUG(L"==== bootfix exit code %d", rc);
    logging::shutdown();
    return rc;
}
