// Command-line parsing.
//
// Convention that keeps us compatible with the original launcher's job of
// forwarding SA:MP arguments to gta_sa.exe:
//
//   * Launcher-only options use the long "--name" form and are consumed here.
//   * Everything else — notably SA:MP's single-dash flags "-c -n <nick>
//     -h <host> -p <port> -z <pass>" — is forwarded verbatim to gta_sa.exe.
//   * "--" forces all following tokens to be forwarded, even "--"-looking ones.
#pragma once

#include <optional>
#include <string>
#include <vector>

struct CliOptions {
    // Launcher behavior switches.
    bool no_inject = false;   // launch GTA but skip DLL injection
    bool dry_run   = false;   // validate + log the plan, do not launch
    bool debug     = false;   // verbose (Debug/Trace) logging
    bool verbose   = false;   // alias for debug, kept for discoverability
    bool console   = false;   // allocate a console and mirror the log to it
    bool json      = false;   // write the log as JSON-lines
    bool wait_idle = false;   // WaitForInputIdle after resume (diagnostic)
    bool kill_on_inject_fail = true;  // terminate GTA if injection fails
    bool help      = false;
    bool version   = false;

    // Runtime monitoring.
    bool monitor   = false;   // --monitor/--gdb: attach debugger, capture crashes
    bool watch     = false;   // --watch: stay attached for the whole session
    bool report    = false;   // --report: emit the report block even on clean exit
    bool all_output = false;  // --all-output: keep plugin log-mirror lines in the report
    unsigned grace_ms = 8000; // --grace <sec>: startup-watchdog window (0 disables)

    // open.mp: also inject omp-client.dll after samp.dll.
    bool omp = false;                      // -omp / --omp

    // Host resolution: remembering the address a hostname answered on (see
    // host_cache.hpp). On by default; the file sits next to the log.
    bool no_host_cache = false;            // --no-host-cache: never read or write it
    std::optional<std::wstring> host_cache_path;  // --host-cache <path>

    // Path overrides (all optional; defaults derive from the launcher directory).
    std::optional<std::wstring> game_dir;  // --game-dir <path>
    std::optional<std::wstring> exe_name;  // --exe <name-or-path>  (default gta_sa.exe)
    std::optional<std::wstring> dll_name;  // --dll <name-or-path>  (default samp.dll)
    std::optional<std::wstring> omp_dll;   // --omp-dll <name-or-path> (default omp-client.dll)
    std::optional<std::wstring> log_path;  // --log <path>          (default launcher.log)

    // Arguments to forward to gta_sa.exe (SA:MP connect flags, etc.).
    std::vector<std::wstring> forwarded;

    // Set when parsing hit a problem; caller should show usage and abort.
    std::optional<std::wstring> parse_error;
};

// Parses the process command line (skips argv[0]).
CliOptions parse_command_line();

// Parses an explicit argv (argv[0] is the program name and is skipped). Exposed
// for testing and reuse.
CliOptions parse_args(const std::vector<std::wstring>& argv);

std::wstring usage_text();
