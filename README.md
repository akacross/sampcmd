# sampcmd — a robust SA:MP launcher for Windows and Wine/Proton

A modern rewrite of `sampcmd.exe`. It launches `gta_sa.exe`, injects `samp.dll`,
and — unlike the original — validates and logs **every** stage so a failure can
never pass silently. It is designed to behave identically on native Windows and
under Wine / Proton / UMU / Lutris / Steam.

## Why the rewrite

The original launcher assumed native Windows behavior and reported success even
when injection failed. Under Wine/Proton the symptom was: `gta_sa.exe` launches,
exits immediately, and the launcher still returns `0`.

This version fixes the correctness bugs from the original and, importantly, two
things the surface-level analysis missed:

1. **It is built 32-bit.** `gta_sa.exe` and `samp.dll` are PE32/i386. A 64-bit
   launcher cannot `CreateRemoteThread` + `LoadLibraryW` into a 32-bit process.
   The build fails loudly if pointed at a 64-bit toolchain.

2. **It sets the child's working directory to the game folder.** GTA:SA exits
   instantly if its current directory isn't the game directory, and under
   Steam/Proton/UMU the inherited cwd usually isn't. The original passed
   `lpCurrentDirectory = NULL` and resolved paths from `GetCurrentDirectory()`;
   this version anchors everything to the launcher's own `.exe` location.

### Correctness fixes carried over from the analysis

- `STARTUPINFOW.cb` is initialized (Wine is stricter than Windows).
- The remote `LoadLibraryW` thread's **exit code (the returned `HMODULE`) is
  checked**; `0` means the DLL did not load, and that is reported, not ignored.
- `VirtualAllocEx` uses `PAGE_READWRITE` (the injected path is data, not code).
- `VirtualAllocEx` result, `WriteProcessMemory` byte count, and the located
  `LoadLibraryW` address are all validated.
- All handles and remote allocations use RAII wrappers (no leaks on any path).
- The command line handed to GTA is **rebuilt** with `gta_sa.exe` as `argv[0]`
  rather than forwarding `sampcmd`'s own command line verbatim.
- `std::filesystem` for all path handling; Unicode throughout.
- Runtime Wine/Proton detection (`wine_get_version`, env markers) — logged.

## Architecture

```
main            -> App (orchestrator)
App             -> Cli, Logger, Diagnostics, WineDetection,
                   ProcessLauncher, Injector, Monitor, ErrorFormatter
Win32Helpers    -> ScopedHandle / RemoteMemory RAII + error formatting
```

One file per module under `src/`.

## Build (Linux -> Windows PE, 32-bit)

Requires the MinGW-w64 i686 toolchain:

```sh
sudo pacman -S mingw-w64-gcc      # Arch / CachyOS
# or: sudo apt install g++-mingw-w64-i686 cmake
```

Then:

```sh
./build.sh                                   # -> build/sampcmd.exe
./build.sh --deploy "/path/to/GTA San Andreas 0.3.7"   # backs up the original first
```

Or manually:

```sh
cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw-i686.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

(On native Windows, configure with any 32-bit MSVC/MinGW generator instead.)

## Usage

```
sampcmd [launcher-options] [--] [gta_sa.exe arguments...]
```

The launcher's own options use `--long` form; single-dash flags and everything
else are forwarded to `gta_sa.exe`, so the existing SA:MP invocation still works:

```sh
sampcmd -c -n MyNick -h 127.0.0.1 -p 7777
```

### open.mp

Pass `-omp` to also inject `omp-client.dll` (from the game folder) **after**
`samp.dll` — open.mp layers on top of an already-loaded samp.dll. The connection
flags are unchanged; `-omp` is consumed by the launcher and not forwarded to GTA.
It may appear **anywhere** on the command line — including after `--` — so it can
sit alongside the connect flags.

```sh
sampcmd -omp -c -n MyNick -h 127.0.0.1 -p 7777
# custom path: sampcmd -omp --omp-dll "C:\path\to\omp-client.dll" -c -n ... -h ... -p ...
```

### Diagnosing the "exits immediately" problem

```sh
# 1. Prove the launcher's view of the world (no launch):
sampcmd --dry-run --debug --console -- -c -n MyNick -h 127.0.0.1 -p 7777

# 2. Does GTA start at all without SA:MP?
sampcmd --no-inject -- -c -n MyNick -h 127.0.0.1 -p 7777

# 3. Full launch with a complete trace:
sampcmd --debug -- -c -n MyNick -h 127.0.0.1 -p 7777
cat "sampcmd.log"   # written next to sampcmd.exe, named after the launcher
```

If injection fails, the log and an error dialog name the exact stage
(`LoadLibraryW returned NULL inside gta_sa.exe`, etc.) instead of GTA vanishing.

## Runtime monitoring (what happened after launch)

The launcher stays attached after starting GTA and reports how the process
behaved. Two tiers:

- **Startup watchdog (default).** Waits a grace window (`--grace`, default 8s) for
  an early exit. If GTA exits/crashes, it decodes the exit status (e.g.
  `0xC0000005 ACCESS_VIOLATION`, `0xC0000135 DLL_NOT_FOUND`) and prints a report.
  If GTA is still healthy after the window, the launcher detaches.
- **`--monitor` / `--gdb` (gdb-lite).** Attaches the Win32 debug API for the whole
  session and additionally captures the **faulting module + offset**, the
  exception chain, the loaded-module list (incl. whether `samp.dll` loaded), and
  the game's own `OutputDebugString` output. Add `--watch` to a plain run to stay
  attached without the debugger.

The report is a compact, delimited block designed to be **pasted back for
debugging**:

```
===== SAMPCMD REPORT (copy everything between the lines) =====
sampcmd     : 2.0.0
os / wine   : 10.0.19045 | Wine 11.12 (Linux 6.x)
target      : gta_sa.exe [samp.dll injected]
outcome     : CRASHED after 2140 ms
exit/status : 0xC0000005  ACCESS_VIOLATION (bad memory access)
fault       : 0xC0000005 ACCESS_VIOLATION at gta_sa.exe+0x28A1B2
exceptions  : 2 seen (showing the fatal one)
modules     : 41 loaded | samp.dll LOADED
last output :
  > SA-MP: ...
log file    : Z:\...\sampcmd.log
===== END REPORT =====
```

```sh
# richest crash info: attach the debugger for the session
sampcmd --monitor -- -c -n MyNick -h 127.0.0.1 -p 7777
```

## Launcher options

| Option | Effect |
| --- | --- |
| `-omp`, `--omp` | Also inject `omp-client.dll` (after `samp.dll`) for open.mp |
| `--omp-dll <name\|path>` | open.mp DLL to inject (default `omp-client.dll`) |
| `--no-inject` | Launch GTA but skip DLL injection |
| `--dry-run` | Validate files + log the plan; do not launch |
| `--debug`, `--verbose` | Verbose (Debug/Trace) logging |
| `--console` | Allocate a console and mirror the log to it (no MessageBoxes) |
| `--json` | Write the log as JSON-lines |
| `--monitor`, `--gdb` | Attach a debugger; report crash module+offset, exceptions, debug output |
| `--watch` | Stay attached until GTA exits, then report (no debugger) |
| `--grace <sec>` | Startup-watchdog window (default 8, `0` disables monitoring) |
| `--report` | Emit the report block even on a clean exit |
| `--wait-idle` | `WaitForInputIdle` after resuming GTA |
| `--no-kill-on-fail` | Resume GTA even if injection fails |
| `--game-dir <path>` | Game directory (default: the launcher's folder) |
| `--exe <name\|path>` | Target executable (default `gta_sa.exe`) |
| `--dll <name\|path>` | DLL to inject (default `samp.dll`) |
| `--log <path>` | Log file (default `<launcher-name>.log`, e.g. `sampcmd.log`) |
| `--help`, `--version` | Info |

The application icon is generated from the game's `samp.png` at build time
(`res/sampcmd.ico`).

## Continuous integration

`.gitlab-ci.yml` builds the 32-bit `sampcmd.exe` on every push using the pinned
llvm-mingw toolchain and publishes it as a job artifact (requires CI/CD + shared
runners enabled on the project).

## License

MIT — see [LICENSE](LICENSE).
