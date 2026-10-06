// Subcommand entry points.  Each returns a process exit code:
//   0 success, 1 the operation failed, 2 usage error, 3 needs an administrator prompt
#pragma once
#include "args.hh"
#include <string>

enum ExitCode { EXIT_OK = 0, EXIT_FAILED = 1, EXIT_USAGE = 2, EXIT_ELEVATION = 3 };

int cmdScan(const Args& args);       // read-only diagnosis of disks / boot sectors / boot files
int cmdFixMbr(const Args& args);     // bootrec /FixMbr  + bootsect /nt60 <x> /mbr
int cmdFixBoot(const Args& args);    // bootrec /FixBoot + bootsect /nt60 <x>
int cmdBcd(const Args& args);        // bcd <verb>: dump, rebuild, set, unset, create, delete, export

// Shared helpers.
// Confirmation for a destructive step (UI-CLI-015).  Returns -1 to proceed, otherwise the
// exit code to return: EXIT_OK after a dry run or a declined prompt, EXIT_FAILED when
// unattended without --force.  `question` is a complete question, e.g.
// "Write BOOTMGR boot code to the MBR of disk 2?"
int confirmOrDryRun(const Args& args, const wchar_t* question);
std::wstring backupDir(const Args& args);                      // created on demand
bool isElevated();
// Logs the "needs an administrator prompt" error and returns false when not elevated.
// Called after argument validation so usage errors still exit 2 (UI-CLI-001).
bool requireElevation(const wchar_t* what);
bool interrupted();                                            // Ctrl-C seen

// Open the store named by --store FILE / --live (cmd_bcd.cpp).
namespace bcd { class Store; }
bool openStoreFromArgs(const Args& args, bool readOnly, bcd::Store* store);
