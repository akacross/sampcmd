#include "windows_prelude.hpp"
#include "cli.hpp"

#include <shellapi.h>
#include <format>

namespace {

// Splits "--name=value" into ("--name", "value"); returns has_value=false if
// there was no '='.
struct Split { std::wstring name; std::wstring value; bool has_value; };
Split split_eq(const std::wstring& arg) {
    auto pos = arg.find(L'=');
    if (pos == std::wstring::npos) return {arg, L"", false};
    return {arg.substr(0, pos), arg.substr(pos + 1), true};
}

} // namespace

CliOptions parse_args(const std::vector<std::wstring>& argv) {
    CliOptions o;
    bool forward_rest = false;

    // Options that take a value, resolved either as "--opt=val" or "--opt val".
    auto take_value = [&](size_t& i, const Split& s, const std::wstring& name,
                          std::optional<std::wstring>& dest) -> bool {
        if (s.has_value) { dest = s.value; return true; }
        if (i + 1 < argv.size()) { dest = argv[++i]; return true; }
        o.parse_error = std::format(L"option {} requires a value", name);
        return false;
    };

    for (size_t i = 1; i < argv.size(); ++i) {
        const std::wstring& raw = argv[i];

        // open.mp toggle. It is a launcher directive (inject omp-client.dll), not a
        // GTA argument, so it is recognized ANYWHERE - even after "--" - and is
        // never forwarded. Accepted with either dash count for convenience.
        if (raw == L"-omp" || raw == L"--omp") { o.omp = true; continue; }

        if (forward_rest) { o.forwarded.push_back(raw); continue; }

        if (raw == L"--") { forward_rest = true; continue; }

        // Only long "--" options are launcher-owned. Single-dash and bare tokens
        // belong to gta_sa.exe.
        if (raw.size() >= 2 && raw[0] == L'-' && raw[1] == L'-') {
            Split s = split_eq(raw);
            const std::wstring& n = s.name;

            if      (n == L"--no-inject") o.no_inject = true;
            else if (n == L"--dry-run")   o.dry_run = true;
            else if (n == L"--debug")     { o.debug = true; o.verbose = true; }
            else if (n == L"--verbose")   { o.verbose = true; o.debug = true; }
            else if (n == L"--console")   o.console = true;
            else if (n == L"--json")      o.json = true;
            else if (n == L"--wait-idle") o.wait_idle = true;
            else if (n == L"--no-kill-on-fail") o.kill_on_inject_fail = false;
            else if (n == L"--monitor" || n == L"--gdb") o.monitor = true;
            else if (n == L"--watch")     o.watch = true;
            else if (n == L"--report")    o.report = true;
            else if (n == L"--all-output") o.all_output = true;
            else if (n == L"--no-host-cache") o.no_host_cache = true;
            else if (n == L"--grace") {
                std::optional<std::wstring> v;
                if (!take_value(i, s, n, v)) return o;
                try { o.grace_ms = static_cast<unsigned>(std::stoul(*v)) * 1000u; }
                catch (...) { o.parse_error = std::format(L"--grace needs a number of seconds, got: {}", *v); return o; }
            }
            else if (n == L"--help")      o.help = true;   // note: -h is SA:MP's host flag, forwarded
            else if (n == L"--version")   o.version = true;
            else if (n == L"--game-dir")  { if (!take_value(i, s, n, o.game_dir)) return o; }
            else if (n == L"--exe")       { if (!take_value(i, s, n, o.exe_name)) return o; }
            else if (n == L"--dll")       { if (!take_value(i, s, n, o.dll_name)) return o; }
            else if (n == L"--omp-dll")   { if (!take_value(i, s, n, o.omp_dll)) return o; }
            else if (n == L"--log")       { if (!take_value(i, s, n, o.log_path)) return o; }
            else if (n == L"--host-cache"){ if (!take_value(i, s, n, o.host_cache_path)) return o; }
            else {
                o.parse_error = std::format(L"unknown option: {}", n);
                return o;
            }
            continue;
        }

        // Anything else is forwarded to gta_sa.exe unchanged.
        o.forwarded.push_back(raw);
    }

    return o;
}

CliOptions parse_command_line() {
    int argc = 0;
    LPWSTR* wargv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    std::vector<std::wstring> argv;
    if (wargv) {
        argv.reserve(argc);
        for (int i = 0; i < argc; ++i) argv.emplace_back(wargv[i]);
        ::LocalFree(wargv);
    }
    return parse_args(argv);
}

std::wstring usage_text() {
    return
        L"sampcmd - SA:MP launcher (Windows / Wine / Proton)\n"
        L"\n"
        L"Usage:\n"
        L"  sampcmd [launcher-options] [-- ] [gta_sa.exe arguments...]\n"
        L"\n"
        L"Launcher options:\n"
        L"  -omp, --omp          Also inject omp-client.dll (after samp.dll) for open.mp\n"
        L"  --omp-dll <name|path> open.mp DLL to inject (default omp-client.dll)\n"
        L"  --no-inject          Launch gta_sa.exe but do not inject samp.dll\n"
        L"  --dry-run            Validate files and log the plan; do not launch\n"
        L"  --debug, --verbose   Verbose logging (Debug/Trace levels)\n"
        L"  --console            Allocate a console and mirror the log to it\n"
        L"  --json               Write the log as JSON-lines\n"
        L"  --monitor, --gdb     Attach a debugger and report crashes (module+offset,\n"
        L"                       exceptions, game debug output) - paste the report back\n"
        L"  --watch              Stay attached until GTA exits, then report\n"
        L"  --grace <seconds>    Startup-watchdog window (default 8, 0 disables)\n"
        L"  --report             Emit the report block even on a clean exit\n"
        L"  --all-output         Keep plugin log-mirror lines (cleo.asi) in the report;\n"
        L"                       they are dropped by default so the game's own output shows\n"
        L"  --wait-idle          WaitForInputIdle after resuming GTA (diagnostic)\n"
        L"  --no-kill-on-fail    Leave GTA running even if injection fails\n"
        L"  --game-dir <path>    Game directory (default: launcher's own folder)\n"
        L"  --exe <name|path>    Target executable (default: gta_sa.exe)\n"
        L"  --dll <name|path>    DLL to inject   (default: samp.dll)\n"
        L"  --log <path>         Log file path   (default: launcher.log)\n"
        L"  --host-cache <path>  Proven -h addresses (default: launcher.hosts, next to the log)\n"
        L"  --no-host-cache      Do not read or write remembered -h addresses\n"
        L"  --help               Show this help\n"
        L"  --version            Show version\n"
        L"\n"
        L"Forwarded to gta_sa.exe (SA:MP connect flags), e.g.:\n"
        L"  sampcmd -c -n MyNick -h 127.0.0.1 -p 7777\n"
        L"  sampcmd --debug --console -- -c -n MyNick -h play.example.net -p 7777\n"
        L"\n"
        L"If -h is a hostname, sampcmd resolves it and forwards an address the\n"
        L"server actually answers on. SA:MP connects to the first address the\n"
        L"resolver returns, and DNS pools often keep a record pointing at a machine\n"
        L"that no longer serves - which fails every attempt, not one in ten.\n"
        L"\n"
        L"The address a host answered on is remembered in launcher.hosts: it is tried\n"
        L"first next launch, reused outright for five minutes, and used as a fallback\n"
        L"when a query goes unanswered (real servers drop 10-15% of them). Add a line\n"
        L"\"host <tab> port <tab> address <tab> pin\" to that file to hard-wire a host.\n";
}
