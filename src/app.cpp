#include "app.hpp"

#include "diagnostics.hpp"
#include "error_formatter.hpp"
#include "injector.hpp"
#include "monitor.hpp"
#include "process_launcher.hpp"
#include "strconv.hpp"
#include "wine_detection.hpp"
#include "win32_helpers.hpp"

#include <cstdint>
#include <cstdio>
#include <format>
#include <functional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr wchar_t kVersion[] = L"2.0.0";

// Quotes a single argument per the CommandLineToArgvW / MSVCRT rules so that
// arguments containing spaces or quotes survive the round trip into gta_sa.exe.
std::wstring quote_arg(const std::wstring& arg) {
    if (!arg.empty() &&
        arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return arg;  // no quoting needed
    }
    std::wstring out;
    out.push_back(L'"');
    for (auto it = arg.begin();; ++it) {
        unsigned backslashes = 0;
        while (it != arg.end() && *it == L'\\') { ++it; ++backslashes; }
        if (it == arg.end()) {
            out.append(backslashes * 2, L'\\');  // escape trailing backslashes
            break;
        } else if (*it == L'"') {
            out.append(backslashes * 2 + 1, L'\\');  // escape backslashes + the quote
            out.push_back(L'"');
        } else {
            out.append(backslashes, L'\\');
            out.push_back(*it);
        }
    }
    out.push_back(L'"');
    return out;
}

// Allocates a console for --console mode and wires up stdout/stderr.
void attach_console() {
    if (::AllocConsole()) {
        // Reopen the CRT streams onto the new console.
        std::freopen("CONOUT$", "w", stdout);
        std::freopen("CONOUT$", "w", stderr);
        std::freopen("CONIN$",  "r", stdin);
        ::SetConsoleOutputCP(CP_UTF8);
    }
}

} // namespace

App::Paths App::resolve_paths(const CliOptions& opts) const {
    fs::path launcher_dir = diagnostics::self_directory();

    Paths p;
    p.game_dir = opts.game_dir ? fs::path(*opts.game_dir) : launcher_dir;

    // exe/dll may be a bare name (resolved under game_dir) or an explicit path.
    auto resolve = [&](const std::optional<std::wstring>& override,
                       const wchar_t* def) -> fs::path {
        fs::path v = override ? fs::path(*override) : fs::path(def);
        return v.is_absolute() ? v : (p.game_dir / v);
    };
    p.exe = resolve(opts.exe_name, L"gta_sa.exe");
    p.dll = resolve(opts.dll_name, L"samp.dll");

    // Log next to the launcher, named after it (sampcmd.exe -> sampcmd.log), so
    // it is easy to find and follows the exe if it is renamed.
    std::wstring log_name = diagnostics::self_executable_path().stem().wstring();
    if (log_name.empty()) log_name = L"sampcmd";
    log_name += L".log";
    p.log = opts.log_path ? fs::path(*opts.log_path) : (launcher_dir / log_name);
    return p;
}

std::wstring App::build_child_command_line(const fs::path& exe,
                                           const std::vector<std::wstring>& forwarded) {
    // argv[0] is the target executable itself (NOT sampcmd), which is the fix for
    // forwarding the original launcher's name into GTA.
    std::wstring cmd = quote_arg(exe.wstring());
    for (const auto& a : forwarded) {
        cmd.push_back(L' ');
        cmd += quote_arg(a);
    }
    return cmd;
}

bool App::require_file(const fs::path& p, std::wstring_view label) {
    const std::wstring s = p.wstring();
    DWORD attrs = ::GetFileAttributesW(s.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        logger_.errorf(L"{} not found: {}", label, s);
        return false;
    }
    logger_.infof(L"{:<14}: {} [ok]", label, s);
    return true;
}

int App::run() {
    CliOptions opts = parse_command_line();

    // --console must happen before the logger opens so mirrored output has a sink.
    if (opts.console) attach_console();

    Paths paths = resolve_paths(opts);

    Logger::Options lo;
    lo.file_path = paths.log.wstring();
    lo.console   = opts.console;
    lo.json      = opts.json;
    lo.min_level = (opts.debug || opts.verbose) ? Logger::Level::Trace : Logger::Level::Info;
    logger_.open(lo);

    const bool gui = !opts.console;  // no MessageBox spam when a console is present
    ErrorFormatter err(logger_, gui);

    logger_.infof(L"sampcmd {} starting", kVersion);
    logger_.infof(L"log file       : {}", paths.log.wstring());

    // Early-exit modes.
    if (opts.parse_error) {
        logger_.errorf(L"argument error: {}", *opts.parse_error);
        err.report(L"Invalid arguments", *opts.parse_error, {{L"Usage", L"see --help"}});
        return 2;
    }
    if (opts.help) {
        if (gui) ::MessageBoxW(nullptr, usage_text().c_str(), L"sampcmd help", MB_OK | MB_ICONINFORMATION);
        else     logger_.info(usage_text());
        return 0;
    }
    if (opts.version) {
        logger_.infof(L"sampcmd version {}", kVersion);
        if (gui) ::MessageBoxW(nullptr, std::format(L"sampcmd {}", kVersion).c_str(),
                               L"sampcmd", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    // Diagnostics + environment detection (always logged).
    diagnostics::log_snapshot(logger_);
    const WineInfo wine = detect_wine(logger_);

    // Validate inputs.
    logger_.section(L"Validation");
    logger_.infof(L"game dir       : {}", paths.game_dir.wstring());
    bool ok = true;
    ok &= require_file(paths.exe, L"gta_sa.exe");
    ok &= require_file(paths.dll, L"samp.dll");
    if (!ok) {
        err.report(L"Missing files",
                   L"Required files were not found. Is the launcher in the game folder?",
                   {
                       {L"Game Dir", paths.game_dir.wstring()},
                       {L"Expected exe", paths.exe.wstring()},
                       {L"Expected dll", paths.dll.wstring()},
                   });
        return 3;
    }

    const std::wstring child_cmd = build_child_command_line(paths.exe, opts.forwarded);
    logger_.infof(L"child cmdline  : {}", child_cmd);
    if (opts.forwarded.empty())
        logger_.warn(L"no SA:MP arguments forwarded (GTA will start without connect info)");

    if (opts.dry_run) {
        logger_.info(L"--dry-run: validation complete, not launching");
        return 0;
    }

    // Launch suspended.
    ProcessLauncher launcher(logger_, err);
    auto proc = launcher.create_suspended(paths.exe, child_cmd, paths.game_dir);
    if (!proc) return 4;

    // Inject (unless disabled).
    if (opts.no_inject) {
        logger_.warn(L"--no-inject: skipping DLL injection");
    } else {
        Injector injector(logger_, err);
        InjectionResult inj = injector.inject(proc->process.get(), proc->pid, paths.dll);
        if (!inj.success) {
            logger_.errorf(L"injection failed at stage: {}", inj.failed_stage);
            if (opts.kill_on_inject_fail) {
                logger_.warn(L"terminating gta_sa.exe so the failure is visible (not a silent exit)");
                ::TerminateProcess(proc->process.get(), 1);
            } else {
                logger_.warn(L"--no-kill-on-fail: resuming GTA anyway (will likely exit on its own)");
                launcher.resume(*proc);
            }
            return 5;
        }
    }

    // Context for any report we might emit.
    monitor::ReportContext rc{
        kVersion,
        diagnostics::os_version_string(),
        wine.is_wine
            ? std::format(L"Wine {} ({} {})", wine.version, wine.host_system, wine.host_release)
            : L"native Windows",
        paths.exe.filename().wstring() +
            (opts.no_inject ? L" [not injected]" : L" [samp.dll injected]"),
        paths.log.wstring(),
    };

    const uint64_t launch_tick = ::GetTickCount64();
    monitor::Result mon;

    if (opts.monitor) {
        // gdb-lite: attach a debugger, then resume from inside the monitor so no
        // early event is missed. Blocks for the whole session.
        mon = monitor::debug_and_wait(proc->process.get(), proc->pid, launch_tick,
                                      [&] { launcher.resume(*proc); }, logger_);
    } else {
        if (!launcher.resume(*proc)) return 6;

        if (opts.wait_idle) {
            logger_.debug(L"WaitForInputIdle...");
            DWORD w = ::WaitForInputIdle(proc->process.get(), 10000);
            logger_.debugf(L"WaitForInputIdle returned {}", w);
        }

        if (opts.grace_ms == 0 && !opts.watch) {
            // Monitoring disabled: fire and forget.
            logger_.section(L"Done");
            logger_.infof(L"gta_sa.exe launched (pid {}){}", proc->pid,
                          opts.no_inject ? L" without injection" : L" with samp.dll injected");
            return 0;
        }
        mon = monitor::watch(proc->process.get(), launch_tick,
                             opts.grace_ms ? opts.grace_ms : 8000, opts.watch, logger_);
    }

    // Emit the paste-ready report when something looks wrong (or on request).
    const bool crashed  = mon.process_exited && monitor::is_crash_status(mon.exit_code);
    const bool abnormal = mon.process_exited && mon.exit_code != 0;
    if (crashed || abnormal || opts.report || opts.monitor) {
        std::wstring report = monitor::format_report(rc, mon);
        logger_.section(L"Report");
        logger_.raw(report);
        if (gui && (crashed || abnormal))
            ::MessageBoxW(nullptr, report.c_str(), L"sampcmd - GTA exited unexpectedly",
                          MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
    }

    logger_.section(L"Done");
    if (!mon.process_exited) {
        logger_.infof(L"gta_sa.exe running (pid {}){}", proc->pid,
                      opts.no_inject ? L" without injection" : L" with samp.dll injected");
        return 0;
    }
    logger_.infof(L"gta_sa.exe (pid {}) ended with status 0x{:08X}", proc->pid, mon.exit_code);
    return (mon.exit_code == 0) ? 0 : 5;
}
