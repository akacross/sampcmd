// Runtime monitoring: what happened to gta_sa.exe *after* it launched.
//
// Two tiers:
//   * watch()          - lightweight startup watchdog. Waits a grace period (or
//                        the whole session) and reports the exit code + duration.
//                        Catches the classic "launches then exits immediately".
//   * debug_and_wait() - "gdb-lite". Attaches the Win32 debug API and records
//                        module loads, exceptions (code + faulting module+offset),
//                        and the game's OutputDebugString output, then reports.
//
// The report is a compact, delimited block designed to be copy-pasted back for
// diagnosis.
#pragma once

#include "windows_prelude.hpp"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class Logger;

namespace monitor {

struct ModuleRec {
    uintptr_t    base = 0;
    uintptr_t    size = 0;
    std::wstring name;
};

struct ExceptionRec {
    DWORD        code = 0;
    uintptr_t    address = 0;
    bool         first_chance = true;
    std::wstring module;         // module the faulting address falls in ("?" if none)
    uintptr_t    module_offset = 0;
};

struct Result {
    bool     process_exited = false;   // did we observe the process end?
    DWORD    exit_code = 0;
    uint64_t run_ms = 0;
    bool     attached = false;         // debugger (gdb-lite) mode was used
    bool     samp_loaded = false;      // samp.dll appeared in the module list
    std::vector<ExceptionRec> exceptions;      // non-breakpoint exceptions seen
    std::vector<std::wstring> debug_strings;   // last few OutputDebugString lines
    std::vector<std::wstring> fault_context;   // ...as they stood at the first hard fault
    size_t lines_after_fault = 0;              // lines recorded since that snapshot
    size_t debug_strings_omitted = 0;          // mirrored log lines kept out of the report
    std::vector<ModuleRec>    modules;
};

// Static facts the report header needs (the launcher knows these).
struct ReportContext {
    std::wstring version;
    std::wstring os;
    std::wstring wine;
    std::wstring target;     // e.g. "gta_sa.exe [samp.dll injected]"
    std::wstring log_path;
};

// Decodes a process exit code / NTSTATUS into a human name.
std::wstring decode_status(DWORD code);

// True if `code` looks like a crash NTSTATUS rather than a plain exit code.
bool is_crash_status(DWORD code);

// Startup watchdog. Waits up to timeout_ms (or forever if wait_full) for exit.
Result watch(HANDLE process, uint64_t launch_tick, DWORD timeout_ms, bool wait_full,
             Logger& log);

// gdb-lite. Call with the target's main thread still SUSPENDED; `resume` is
// invoked once the debugger is attached so no early event is missed. Blocks
// until the process exits (whole session).
//
// keep_all_output: report every OutputDebugString line, including the ones a
// third-party log mirror (cleo.asi) floods the debug channel with. They are
// always written to the trace log either way.
Result debug_and_wait(HANDLE process, DWORD pid, uint64_t launch_tick,
                      const std::function<void()>& resume, Logger& log,
                      bool keep_all_output = false);

// Builds the paste-ready report block.
std::wstring format_report(const ReportContext& ctx, const Result& r);

} // namespace monitor
