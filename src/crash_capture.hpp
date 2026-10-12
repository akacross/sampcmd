// Crash capture: what to keep from a hard fault while the faulting thread is
// still stopped on it (the first-chance debug event - the only moment its
// registers are the fault's own).
//
//   * the block SA-MP's own crash dialog shows (samp.dll's handler format,
//     byte for byte: registers, EFLAGS, 0x280 bytes of stack, SCM op), so a
//     report looks the same whether sampcmd or SA-MP caught the crash;
//   * the return addresses on that stack, as module+offset - what the dialog
//     never said and what a crash actually needs;
//   * a minidump (dbghelp MiniDumpWriteDump, loaded at run time) next to the
//     log, for anything the text cannot answer - a heap corrupted earlier.
#pragma once

#include "windows_prelude.hpp"
#include "monitor.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace crash {

using monitor::DumpKind;

struct Capture {
    bool         taken = false;
    std::vector<std::wstring> samp_lines;   // SA-MP's crash text, one line each
    std::vector<std::wstring> return_addrs; // "+0x0010  d3d9.dll+0x1A6000" for stack words in code
    std::wstring dump_path;                 // the .dmp written; empty when none
    std::wstring dump_error;                // why there is none, when one was wanted
    uint64_t     dump_bytes = 0;
};

// Reads the faulting thread and writes the dump. `thread` must be the debug
// thread handle (THREAD_GET_CONTEXT); without it there is no register block,
// but the dump is still attempted. Never throws; every failure is recorded.
Capture capture(HANDLE process, DWORD pid, HANDLE thread, DWORD tid,
                const EXCEPTION_RECORD& er, const std::vector<monitor::ModuleRec>& modules,
                DumpKind kind, const std::wstring& dump_dir);

// The dumps this writes are named sampcmd-crash-<date>-<time>.dmp; all but the
// newest `keep` of them in `dir` are deleted (5 minidumps, 2 full dumps).
void prune_dumps(const std::wstring& dir, size_t keep);

} // namespace crash
