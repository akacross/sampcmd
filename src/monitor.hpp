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

    // Access violations also name the pointer that could not be accessed. That
    // is what separates a null pointer from a corrupted one from a freed block,
    // and it costs nothing: the debugger hands it to us in the same record.
    bool         access_known = false;
    ULONG_PTR    access_kind = 0;      // 0 read, 1 write, 8 DEP violation
    uintptr_t    access_address = 0;
    std::wstring access_module;        // module that address lands in, empty if none
    uintptr_t    access_offset = 0;
};

// One-line description of what an access violation touched, e.g.
// "writing 0x00000000 (null)" or "reading 0x02C70334 (samp.dll+0x150334)".
// Empty for exceptions that carry no access information.
std::wstring describe_access(const ExceptionRec& ex);

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
    // Debug breaks outside ntdll (DebugBreak, __debugbreak, int3): code that
    // stops to say why just before it aborts. Lanes does this: FormatMessage,
    // fprintf to a stderr the game does not have, int3, abort() - and the
    // abort alone is an anonymous __fastfail. Each line names the module and
    // the text the registers and the stack held at the break.
    std::vector<std::wstring> breaks;
    std::vector<ModuleRec>    modules;

    // The first hard fault, read while its thread was still stopped on it
    // (crash_capture): what SA-MP's own crash dialog shows, the code
    // addresses on that stack, and the minidump.
    std::vector<std::wstring> crash_text;
    std::vector<std::wstring> crash_returns;
    std::wstring dump_path;
    std::wstring dump_error;
    uint64_t     dump_bytes = 0;
    // Detached at that fault so the game's own unhandled-exception filter -
    // SA-MP's crash dialog - could run. A debugger being attached is exactly
    // what makes Windows (and Wine) skip that filter.
    bool         handed_over = false;
};

enum class DumpKind { none, mini, full };

struct DebugOptions {
    // Report every OutputDebugString line, including the ones a third-party
    // log mirror (cleo.asi) floods the debug channel with. They are always
    // written to the trace log either way.
    bool         keep_all_output = false;
    // At the first hard fault: capture, then detach and let the game's own
    // crash handler run (SA-MP's dialog). Off: stay attached and terminate the
    // game once the fault proves unhandled (the 2.3 behaviour).
    bool         crash_dialog = true;
    DumpKind     dump = DumpKind::mini;
    std::wstring dump_dir;    // where sampcmd-crash-*.dmp go (the log's folder)
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
// See DebugOptions for what can be chosen.
Result debug_and_wait(HANDLE process, DWORD pid, uint64_t launch_tick,
                      const std::function<void()>& resume, Logger& log,
                      const DebugOptions& opts = {});

// Builds the paste-ready report block. `compact` leaves out the stack dump
// (the message box; the log always has all of it).
std::wstring format_report(const ReportContext& ctx, const Result& r, bool compact = false);

} // namespace monitor
