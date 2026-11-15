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

### Hostnames in `-h` (dead DNS pool members)

If `-h` is a hostname rather than an IP, sampcmd resolves it and forwards **an
address the server actually answers on**.

This exists because of a failure that looks nothing like DNS. SA:MP connects to
the *first* address the resolver hands it, and community servers publish
round-robin A-record pools whose members outlive the machines behind them. Wine
resolves through `getaddrinfo`, whose RFC 3484 ordering is stable for a given
client — so if the dead member sorts first you do not get an intermittent
failure, you get `Failed to connect to host` on every single attempt, on a
hostname that looks perfectly healthy everywhere else.

Resolving alone does not fix it (the stock `samp.exe` resolves too, and picks
the same dead address), so sampcmd **proves** the address first: it sends the
same info query a server browser sends and takes the first candidate that
answers.

```
[Host resolve]
host           : s2.gta-mp.cz (port 7777)
resolved       : 8 address(es)
  178.32.234.16  - no reply in 700 ms (attempt 1 of 3)
  51.254.21.27   - answered in 117 ms  <- using this
WARN 1 address(es) ahead of 51.254.21.27 did not answer - the DNS pool has dead members
```

Cost is one UDP round-trip (~120–250 ms) in the normal case, since the first
address usually answers. It never makes things worse: an IP literal, a missing
`-h`, a resolver failure, or a pool where nothing answers all fall back to
forwarding the hostname exactly as before.

#### One query is not proof — retries and `sampcmd.hosts`

A SA:MP query is a single UDP datagram with no retransmission, and real servers
drop them. Measured 2026-08-16, 20 queries per run, paced and unpaced:

| server | address | answered | RTT |
| --- | --- | --- | --- |
| `samp.hzgaming.net` | 213.32.6.232 | 17/20, 18/20 | ~118 ms |
| `s2.gta-mp.cz` | 51.254.21.27 | 20/20 | ~118 ms |

So hzgaming — a live server with a *single* A record, answering in a sixth of the
probe timeout — looked dead about one launch in seven, and the launcher then
logged `no address answered a SA:MP query` and forwarded the hostname. Nothing
was wrong with the timeout; the packet simply never came back.

Two things follow. The probe now walks the pool **three times** instead of once,
which takes a false "dead" from ~1/7 to ~1/350 for that server and costs nothing
when an address answers. And every proven address is written to
**`sampcmd.hosts`**, next to the log:

```
# host <tab> port <tab> address <tab> proven-at <tab> rtt_ms <tab> fails <tab> pool
samp.hzgaming.net	7777	213.32.6.232	1786915058	118	0	213.32.6.232
```

That file is used four ways:

- **Tried first.** The address that answered last time goes to the head of the
  probe queue, so a bad DNS order costs nothing after the first launch.
- **Trusted for 5 minutes.** Relaunching the server you were just on (usually
  after a crash) skips DNS and the probe entirely — `cache hit : 213.32.6.232 -
  answered 15 s ago in 118 ms; skipping DNS and probe`.
- **Fallback for a day.** If nothing answers this launch but this host answered
  within the last 24 hours *and* DNS still lists that address, it is used anyway
  — silence is far more likely to be a dropped datagram than a dead server. After
  3 such launches with no fresh proof, the entry is forgotten.
- **Survives a DNS outage.** If the name stops resolving, the addresses from the
  last successful lookup are probed instead of giving up.

Add a line by hand to pin a host permanently — no DNS, no probe, no expiry:

```
myserver.net	7777	203.0.113.10	pin
```

`--host-cache <path>` moves the file; `--no-host-cache` disables reading and
writing it.

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
sampcmd     : 2.4.1
os / wine   : 10.0.19045 | Wine 11.12 (Linux 6.x)
target      : gta_sa.exe [samp.dll injected]
outcome     : CRASHED after 2140 ms
exit/status : 0xC0000005  ACCESS_VIOLATION (bad memory access)
fault       : 0xC0000005 ACCESS_VIOLATION at gta_sa.exe+0x28A1B2
exceptions  : 2 seen (showing the fatal one)
modules     : 41 loaded | samp.dll LOADED
output@fault:
  > SA-MP: ...
  ... then:
  > SA-MP: ...
omitted     : 37 mirrored plugin log line(s), e.g. cleo.asi (--all-output keeps them)
log file    : Z:\...\sampcmd.log
===== END REPORT =====
```

### Crashes: SA-MP's dialog, its text, and a dump (2.4)

A debugger being attached is exactly what makes Windows and Wine skip a
program's own unhandled-exception filter, so under `--monitor` SA-MP's crash
dialog used to never appear: the fault came to sampcmd instead, and sampcmd
ended the game.

Now, at the **first hard fault** (an access violation, an illegal instruction -
`0xC0000xxx`), while the faulting thread is still stopped on it, sampcmd:

1. reads its registers and stack and writes **what SA-MP's crash dialog shows**,
   in samp.dll's own format (`Exception At Address`, `Base`, the registers,
   `EFLAGS`, the 0x280 bytes of stack, and for 0.3.7-R5 `SCM Op / lDbg /
   LastRendObj` and `Game Version`), into the log and the report;
2. lists the **code addresses on that stack** as `module+offset` - the likely
   callers, which the dialog never named. Modules are named from the loader's
   own list read at the fault (2.4.1): Wine loads, unloads and reloads some
   DLLs at one base, and a record kept from the load events alone named
   whole ranges after a DLL that had left;
3. writes a **minidump** next to the log, `sampcmd-crash-<date>-<time>.dmp`
   (the faulting thread's registers and stack, the module list, the code
   around the fault; the newest five are kept). `--full-dump` writes the
   game's whole memory instead (hundreds of MB to a few GB; the newest two are
   kept) - what a heap corrupted earlier needs. `--no-dump` writes none.
4. **lets go of the game** (`DebugActiveProcessStop` with the exception still
   pending), so the fault is dispatched as if no debugger had been there: the
   game's own handlers, then SA-MP's crash dialog. sampcmd waits for the game
   to end and reports its exit status. A fault the game handles just carries
   on, unwatched for the rest of the session.

`--no-crash-dialog` keeps the 2.3 behaviour: stay attached, and end the game
once the fault proves unhandled (steps 1-3 still happen).

A `__fastfail` (`0xC0000409`: `abort()`, as from `std::terminate`, or a failed
`/GS` stack cookie) never has a first chance: it reaches the debugger
last-chance only. Since 2.4.2 sampcmd takes steps 1-3 there too, before ending
the game, and the log names the fast fail code. The code on the stack is read
from 4 KB of it, not only SA-MP's 0x280 bytes, since an `abort()` sits deep
under the C runtime's frames.

The report then carries, after the fault line:

```
handed over : sampcmd let go at the fault so the game's own crash handler (SA-MP's dialog) could run
dump        : S:\Games\samp-0.3.7\sampcmd-crash-20261012-023521.dmp (2.4 MB)
code on the stack (nearest the fault first):
  +000C  d3d9.dll+0x1A5F80
  ...
SA-MP crash info:
  SA-MP 0.3.7-R5
  Exception At Address: 0x623E63E9
  Base: 0x02B20000
  Registers:
  EAX: 0x...	EBX: 0x...	ECX: 0x...	EDX: 0x...
  ...
```

The message box leaves out the 40-line stack dump; the log has all of it.

A bad handle passed to CloseHandle and similar calls raises
`0xC0000008` (`STATUS_INVALID_HANDLE`) or `0xC0000235` only while a debugger is
attached; without one the call just fails. Since 2.4.3 sampcmd logs these and
continues, as the game would without it, instead of handing them to the game
as a crash: at exit, CLEO 4's IntOperations.cleo closes a dead handle, and the
crash path then hung the game during its DLL teardown.

An access violation on reserved but uncommitted memory is passed to the game
on its first chance (since 2.4.4). Open Limit Adjuster 2.0's unlimited arrays
reserve address space and commit each page from a vectored exception handler
on first touch; sampcmd hears of the fault before that handler runs and used to
report it as a crash (in `CCoronas::Init`). If nothing commits the page, the
fault comes back as last chance and is reported as before.

### A note on the captured output

`OutputDebugString` is a process-wide channel, and attaching the debugger makes
sampcmd its only listener for the session. Anything loaded into GTA can write to
it — notably **cleo.asi**, which mirrors every `cleo.log` line into the debug
channel. Left alone, that flood evicts the game's own output from the report and
buries the crash under CLEO's shutdown chatter, so those mirrored lines are
dropped from the report (`--all-output` keeps them; the trace log under
`--debug` always has every line, and `cleo.log` has them anyway).

When a hard fault is seen, the report shows the output **as it stood at the
fault** (`output@fault`) plus a few trailing lines, rather than only the last
lines before exit.

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
| `--all-output` | Keep plugin log-mirror lines (`cleo.asi`) in the report instead of dropping them |
| `--no-crash-dialog` | With `--monitor`: stay attached at a crash (SA-MP's dialog then never shows); default is to record the crash and let go so it does |
| `--full-dump` | With `--monitor`: a full-memory crash dump instead of the default minidump |
| `--no-dump` | With `--monitor`: no crash dump |
| `--wait-idle` | `WaitForInputIdle` after resuming GTA |
| `--no-kill-on-fail` | Resume GTA even if injection fails |
| `--game-dir <path>` | Game directory (default: the launcher's folder) |
| `--exe <name\|path>` | Target executable (default `gta_sa.exe`) |
| `--dll <name\|path>` | DLL to inject (default `samp.dll`) |
| `--log <path>` | Log file (default `<launcher-name>.log`, e.g. `sampcmd.log`) |
| `--host-cache <path>` | Proven `-h` addresses (default `<launcher-name>.hosts`, next to the log) |
| `--no-host-cache` | Do not read or write remembered `-h` addresses |
| `--help`, `--version` | Info |

The application icon is generated from the game's `samp.png` at build time
(`res/sampcmd.ico`).

## Continuous integration

`.gitlab-ci.yml` builds the 32-bit `sampcmd.exe` on every push using the pinned
llvm-mingw toolchain and publishes it as a job artifact (requires CI/CD + shared
runners enabled on the project).

## License

MIT — see [LICENSE](LICENSE).
