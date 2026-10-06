# bootfix

Windows boot repair, reconstructed from the Windows 10 (17763) `bootrec`, `bootsect`, `bcdboot` and `bcdedit`
binaries: MBR and volume boot code, BCD stores. Command line plus an optional Dear ImGui window (`--gui`).

```
git clone --recurse-submodules git@github.com:Pugnator/bootfix.git
cmake -S bootfix -B bootfix/build -G "Visual Studio 17 2022" -A x64
cmake --build bootfix/build --config Release          # -DBOOTFIX_GUI=OFF for the CLI alone
```

MSVC only, C++11, x64, static CRT. Run from an administrator prompt.

```
bootfix scan [--disk N] [--json]
bootfix fixmbr --disk N [--nt52] [-n] [-f]
bootfix fixboot VOLUME... [--nt52] [--from-backup] [--dismount] [-n] [-f]
bootfix fixboot --all
bootfix bcd dump [--store FILE | --live] [--json]
bootfix bcd rebuild [WINDIR...] [--esp VOLUME] [--firmware UEFI|BIOS] [--recreate] [-n] [-f]
bootfix bcd set --entry ID NAME=VALUE... (--store FILE | --live)
bootfix bcd unset | create --type TYPE | delete --entry ID | export FILE
bootfix --gui
```

`bootfix help <command>` has the details. The log (full trace, hex dumps of every sector read or written) and the
backups of everything overwritten are in `%LOCALAPPDATA%\bootfix`; `--log` and `--backup-dir` override.

Exit codes: 0 ok, 1 failed, 2 usage, 3 needs an administrator prompt.

## License

Copyright (C) 2026 Pugnator. GNU General Public License v3.0 or later (`SPDX-License-Identifier: GPL-3.0-or-later`),
see `LICENSE`. Dear ImGui (`third_party/imgui`) is MIT-licensed by its own authors. The boot-code blobs in
`bootcode/` are byte copies of Microsoft's boot sectors as shipped in Windows 10 and are not covered by this license.
