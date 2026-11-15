#include "windows_prelude.hpp"
#include "monitor.hpp"
#include "logger.hpp"
#include "win32_helpers.hpp"
#include "crash_capture.hpp"

#include <psapi.h>

#include <algorithm>
#include <format>
#include <string_view>
#include <unordered_map>

namespace monitor {

namespace {

constexpr size_t kMaxDebugStrings = 8;
constexpr size_t kMaxExceptions   = 6;
constexpr size_t kMaxTailAfterFault = 4;  // trailing lines shown alongside fault context
constexpr size_t kMaxBreaks = 4;          // debug breaks kept for the report

// True for a line that looks like a plugin mirroring its own log file into the
// debug channel: CLEO (cleo.asi) passes every cleo.log line to
// OutputDebugString, prefixed "dd/mm/yyyy HH:MM:SS.mmm ". It emits dozens of
// them per session - enough to evict everything the game itself said from the
// ring buffer - so they stay out of the report unless --all-output is given.
// The trace log keeps them regardless, and cleo.log has them anyway.
bool is_mirrored_log_line(std::wstring_view s) {
    auto digits = [s](size_t off, size_t n) {
        for (size_t i = off; i < off + n; ++i)
            if (s[i] < L'0' || s[i] > L'9') return false;
        return true;
    };
    //                 0         1         2
    //                 0123456789012345678901234
    // shape required: dd/mm/yyyy HH:MM:SS.mmm<sp>
    if (s.size() < 24) return false;
    return digits(0, 2) && s[2] == L'/' && digits(3, 2) && s[5] == L'/' &&
           digits(6, 4) && s[10] == L' ' && digits(11, 2) && s[13] == L':' &&
           digits(14, 2) && s[16] == L':' && digits(17, 2) && s[19] == L'.' &&
           digits(20, 3) && s[23] == L' ';
}

// Resolve a module's on-disk name from its base address in the target.
std::wstring module_name(HANDLE process, uintptr_t base) {
    wchar_t path[MAX_PATH];
    if (::GetModuleFileNameExW(process, reinterpret_cast<HMODULE>(base), path,
                               static_cast<DWORD>(std::size(path)))) {
        std::wstring full(path);
        auto slash = full.find_last_of(L"\\/");
        return slash == std::wstring::npos ? full : full.substr(slash + 1);
    }
    // Wine sometimes hides the name behind GetModuleFileNameEx; fall back to the
    // mapped file path.
    if (::GetMappedFileNameW(process, reinterpret_cast<void*>(base), path,
                             static_cast<DWORD>(std::size(path)))) {
        std::wstring full(path);
        auto slash = full.find_last_of(L"\\/");
        return slash == std::wstring::npos ? full : full.substr(slash + 1);
    }
    return L"?";
}

uintptr_t module_size(HANDLE process, uintptr_t base) {
    MODULEINFO mi{};
    if (::GetModuleInformation(process, reinterpret_cast<HMODULE>(base), &mi, sizeof(mi)))
        return mi.SizeOfImage;
    return 0;
}

// Printable ASCII starting at `addr` in the target, if there are at least
// four characters of it; empty otherwise.
std::wstring remote_text(HANDLE process, uintptr_t addr) {
    if (addr < 0x10000) return {};
    char buf[96];
    SIZE_T got = 0;
    if (!::ReadProcessMemory(process, reinterpret_cast<LPCVOID>(addr), buf, sizeof(buf), &got)) {
        // the string may end near a page edge: try a shorter read
        if (!::ReadProcessMemory(process, reinterpret_cast<LPCVOID>(addr), buf, 16, &got)) return {};
    }
    std::wstring out;
    for (SIZE_T i = 0; i < got; ++i) {
        const unsigned char c = static_cast<unsigned char>(buf[i]);
        if (c == 0) break;
        if (c < 0x20 || c > 0x7E) return {};
        out += static_cast<wchar_t>(c);
    }
    return out.size() >= 4 ? out : std::wstring{};
}

// The runs of readable text (six characters or more) in the top of a stack:
// what a function formatted into a local buffer before it stopped.
std::wstring stack_text(HANDLE process, uintptr_t esp) {
    char buf[0x400];
    SIZE_T got = 0;
    if (!::ReadProcessMemory(process, reinterpret_cast<LPCVOID>(esp), buf, sizeof(buf), &got))
        return {};
    std::wstring out, run;
    auto flush = [&] {
        while (!run.empty() && run.back() == L' ') run.pop_back();
        // text, not stack words that happen to be printable: four letters or more
        const auto letters = std::count_if(run.begin(), run.end(), [](wchar_t c) {
            return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z'); });
        if (run.size() >= 6 && letters >= 4) {
            if (!out.empty()) out += L" | ";
            out += L"'" + run + L"'";
        }
        run.clear();
    };
    for (SIZE_T i = 0; i < got && out.size() < 400; ++i) {
        const unsigned char c = static_cast<unsigned char>(buf[i]);
        if (c >= 0x20 && c <= 0x7E) run += static_cast<wchar_t>(c);
        else flush();
    }
    flush();
    return out;
}

// One line for a debug break: where, the registers that point at text, and
// the text on the stack.
std::wstring describe_break(HANDLE process, HANDLE thread, const ExceptionRec& x) {
    std::wstring line = std::format(L"int3 at {}+0x{:X}", x.module, x.module_offset);
    if (!thread) return line;
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (!::GetThreadContext(thread, &ctx)) return line;
    const std::pair<const wchar_t*, DWORD> regs[] = {
        {L"eax", ctx.Eax}, {L"ebx", ctx.Ebx}, {L"ecx", ctx.Ecx}, {L"edx", ctx.Edx},
        {L"esi", ctx.Esi}, {L"edi", ctx.Edi}};
    for (const auto& [name, value] : regs)
        if (std::wstring t = remote_text(process, value); !t.empty())
            line += std::format(L" {}->'{}'", name, t);
    if (std::wstring t = stack_text(process, ctx.Esp); !t.empty())
        line += L" stack: " + t;
    return line;
}

// A load at a base that is already recorded replaces that record: Wine loads,
// unloads and reloads some DLLs at one base (bcrypt, rsaenh, crypt32), and a
// stale record there - read for a size after something else took the range -
// claimed whole neighbourhoods of addresses for the wrong module.
void record_module(std::vector<ModuleRec>& mods, HANDLE process, uintptr_t base) {
    if (!base) return;
    ModuleRec m;
    m.base = base;
    m.size = module_size(process, base);
    m.name = module_name(process, base);
    for (auto& old : mods)
        if (old.base == base) { old = std::move(m); return; }
    mods.push_back(std::move(m));
}

void forget_module(std::vector<ModuleRec>& mods, uintptr_t base) {
    std::erase_if(mods, [base](const ModuleRec& m) { return m.base == base; });
}

// The loader's own list, read at the fault: by then every module is in it with
// its real size, which the load events often could not get (GetModuleInformation
// has nothing yet at LOAD_DLL time under Wine). Kept as it was if this fails.
void refresh_modules(std::vector<ModuleRec>& mods, HANDLE process) {
    HMODULE handles[1024];
    DWORD needed = 0;
    if (!::EnumProcessModulesEx(process, handles, sizeof(handles), &needed, LIST_MODULES_ALL))
        return;
    const size_t count = std::min<size_t>(needed / sizeof(HMODULE), std::size(handles));
    if (count == 0) return;
    std::vector<ModuleRec> fresh;
    fresh.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const auto base = reinterpret_cast<uintptr_t>(handles[i]);
        ModuleRec m;
        m.base = base;
        m.size = module_size(process, base);
        m.name = module_name(process, base);
        fresh.push_back(std::move(m));
    }
    mods = std::move(fresh);
}

// Finds the module an address falls in, or nullptr.
const ModuleRec* find_module(const std::vector<ModuleRec>& mods, uintptr_t addr) {
    const ModuleRec* best = nullptr;
    for (const auto& m : mods) {
        if (m.size) {
            if (addr >= m.base && addr < m.base + m.size) return &m;
        } else if (addr >= m.base && (!best || m.base > best->base)) {
            best = &m;  // no size known: nearest base below the address
        }
    }
    return best;
}

// Maps a faulting address to the containing module.
void locate(const std::vector<ModuleRec>& mods, ExceptionRec& ex) {
    if (const ModuleRec* best = find_module(mods, ex.address)) {
        ex.module = best->name;
        ex.module_offset = ex.address - best->base;
    } else {
        ex.module = L"?";
        ex.module_offset = ex.address;
    }
}

// Pulls the inaccessible pointer out of an access violation's record. The fault
// address says which instruction died; this says what it touched, which is
// usually the difference between a diagnosis and a guess.
void record_access(const std::vector<ModuleRec>& mods,
                   const EXCEPTION_RECORD& er, ExceptionRec& ex) {
    if (er.ExceptionCode != EXCEPTION_ACCESS_VIOLATION &&
        er.ExceptionCode != EXCEPTION_IN_PAGE_ERROR)
        return;
    if (er.NumberParameters < 2) return;

    ex.access_known = true;
    ex.access_kind = er.ExceptionInformation[0];
    ex.access_address = static_cast<uintptr_t>(er.ExceptionInformation[1]);

    // Only claim a module when the address is genuinely inside one. The
    // "nearest base below" fallback is fine for a code address, but on a wild
    // data pointer it would invent an attribution.
    const ModuleRec* m = find_module(mods, ex.access_address);
    if (m && m->size && ex.access_address < m->base + m->size) {
        ex.access_module = m->name;
        ex.access_offset = ex.access_address - m->base;
    }
}

std::wstring read_debug_string(HANDLE process, const OUTPUT_DEBUG_STRING_INFO& info) {
    DWORD n = info.nDebugStringLength;
    if (n == 0 || n > 4096) n = std::min<DWORD>(n, 4096);
    std::string bytes(n, '\0');
    SIZE_T got = 0;
    if (!::ReadProcessMemory(process, info.lpDebugStringData, bytes.data(), n, &got))
        return {};
    bytes.resize(got);
    std::wstring out;
    if (info.fUnicode) {
        out.assign(reinterpret_cast<const wchar_t*>(bytes.data()), got / sizeof(wchar_t));
    } else {
        out.reserve(got);
        for (char c : bytes) out.push_back(static_cast<unsigned char>(c));
    }
    // Trim trailing whitespace/newlines the caller doesn't want.
    while (!out.empty() && (out.back() == L'\r' || out.back() == L'\n' ||
                            out.back() == L' ' || out.back() == L'\0'))
        out.pop_back();
    return out;
}

} // namespace

bool is_crash_status(DWORD code) {
    // NTSTATUS error severity (top two bits set) => a crash/abnormal termination.
    // Plain application exit codes (0, small ints) are not crashes.
    return (code & 0xC0000000u) == 0xC0000000u ||
           code == 0x40000015u /* FATAL_APP_EXIT */;
}

std::wstring describe_access(const ExceptionRec& ex) {
    if (!ex.access_known) return {};

    const wchar_t* verb = ex.access_kind == 1 ? L"writing"
                        : ex.access_kind == 8 ? L"executing (DEP)"
                                              : L"reading";
    std::wstring where;
    if (!ex.access_module.empty())
        where = std::format(L" ({}+0x{:X})", ex.access_module, ex.access_offset);
    else if (ex.access_address < 0x10000)
        where = L" (low address - a null, or a count/index used as a pointer)";

    return std::format(L"{} 0x{:08X}{}", verb, ex.access_address, where);
}

// The common __fastfail codes (winnt.h FAST_FAIL_*).
static std::wstring describe_fast_fail(ULONG_PTR code) {
    switch (code) {
        case 2:  return L" (FAST_FAIL_STACK_COOKIE_CHECK_FAILURE: a /GS stack cookie was overwritten)";
        case 5:  return L" (FAST_FAIL_INVALID_ARG: the CRT's invalid parameter handler)";
        case 7:  return L" (FAST_FAIL_FATAL_APP_EXIT: abort(), as from std::terminate)";
        case 10: return L" (FAST_FAIL_GUARD_ICALL_CHECK_FAILURE: a CFG check failed)";
        default: return L"";
    }
}

std::wstring decode_status(DWORD code) {
    switch (code) {
        case 0x00000000: return L"OK (normal exit)";
        case 0x00000001: return L"exit code 1";
        case 0x00000003: return L"exit code 3 (abort/CRT)";
        case 0x40000015: return L"FATAL_APP_EXIT";
        case 0x40010005: return L"CONTROL_C_EXIT";
        case 0x80000003: return L"BREAKPOINT";
        case 0x80000004: return L"SINGLE_STEP";
        case 0xC0000005: return L"ACCESS_VIOLATION (bad memory access)";
        case 0xC0000006: return L"IN_PAGE_ERROR";
        case 0xC000001D: return L"ILLEGAL_INSTRUCTION";
        case 0xC0000025: return L"NONCONTINUABLE_EXCEPTION";
        case 0xC0000094: return L"INTEGER_DIVIDE_BY_ZERO";
        case 0xC0000095: return L"INTEGER_OVERFLOW";
        case 0xC00000FD: return L"STACK_OVERFLOW";
        case 0xC0000135: return L"DLL_NOT_FOUND (a required DLL is missing)";
        case 0xC0000139: return L"ENTRYPOINT_NOT_FOUND";
        case 0xC0000142: return L"DLL_INIT_FAILED (a DLL's init routine failed)";
        case 0xC0000374: return L"HEAP_CORRUPTION";
        case 0xC0000409: return L"STACK_BUFFER_OVERRUN / __fastfail";
        case 0xC000041D: return L"FATAL_USER_CALLBACK_EXCEPTION";
        default:
            if (is_crash_status(code))
                return std::format(L"crash status 0x{:08X}", code);
            return std::format(L"exit code {} (0x{:08X})", code, code);
    }
}

Result watch(HANDLE process, uint64_t launch_tick, DWORD timeout_ms, bool wait_full,
             Logger& log) {
    Result r;
    log.section(L"Monitor");
    if (wait_full)
        log.info(L"watching process until it exits (--watch)");
    else
        log.infof(L"startup watchdog: watching for early exit ({} ms)", timeout_ms);

    const DWORD w = ::WaitForSingleObject(process, wait_full ? INFINITE : timeout_ms);
    r.run_ms = ::GetTickCount64() - launch_tick;

    if (w == WAIT_OBJECT_0) {
        r.process_exited = true;
        DWORD code = 0;
        ::GetExitCodeProcess(process, &code);
        r.exit_code = code;
        log.infof(L"process exited after {} ms with status {} ({})",
                  r.run_ms, code, decode_status(code));
    } else if (w == WAIT_TIMEOUT) {
        log.infof(L"process still running after {} ms - looks healthy, detaching",
                  r.run_ms);
    } else {
        log.warnf(L"WaitForSingleObject returned 0x{:08X}", w);
    }
    return r;
}

Result debug_and_wait(HANDLE process, DWORD pid, uint64_t launch_tick,
                      const std::function<void()>& resume, Logger& log,
                      const DebugOptions& opts) {
    const bool keep_all_output = opts.keep_all_output;
    Result r;
    r.attached = true;
    log.section(L"Monitor (gdb-lite)");

    // Keep the game alive if the launcher/debugger goes away.
    ::DebugSetProcessKillOnExit(FALSE);

    if (!::DebugActiveProcess(pid)) {
        log.warnf(L"DebugActiveProcess failed: {} - falling back to plain watch",
                  win32::last_error());
        resume();
        return watch(process, launch_tick, 10000, true, log);
    }
    log.infof(L"attached debugger to pid {}", pid);

    // Now that we are attached, let the game run.
    resume();

    // Tracks a repeating first-chance fault so we can break the "unhandled
    // access-violation re-delivered forever" loop that a debugger otherwise hits.
    uintptr_t last_fault_addr = 0;
    DWORD     last_fault_code = 0;
    int       fault_repeats   = 0;
    bool      terminating     = false;
    // The debug output around the first hard fault is what actually explains a
    // crash; the ring buffer alone would have rotated it away by exit time.
    bool      fault_context_taken = false;
    // The crash capture (registers, stack, dump) is taken once a session.
    bool      crash_captured = false;
    // Thread handles by id, from the create events (the debugger owns them),
    // so a debug break can read its thread's registers.
    std::unordered_map<DWORD, HANDLE> threads;

    // Read the faulting thread (SA-MP's crash text, the code on its stack) and
    // write the dump, while that thread is still stopped on the fault.
    auto capture_crash = [&](const DEBUG_EVENT& ev, const EXCEPTION_RECORD& er) {
        crash_captured = true;
        auto t = threads.find(ev.dwThreadId);
        crash::Capture c = crash::capture(process, pid,
                                          t == threads.end() ? nullptr : t->second,
                                          ev.dwThreadId, er, r.modules, opts.dump,
                                          opts.dump_dir);
        r.crash_text = std::move(c.samp_lines);
        r.crash_returns = std::move(c.return_addrs);
        r.dump_path = std::move(c.dump_path);
        r.dump_error = std::move(c.dump_error);
        r.dump_bytes = c.dump_bytes;
        if (!r.dump_path.empty())
            log.infof(L"crash dump written: {} ({} KB)", r.dump_path, r.dump_bytes / 1024);
        else if (!r.dump_error.empty())
            log.warnf(L"no crash dump: {}", r.dump_error);
        for (const auto& line : r.crash_text) log.infof(L"crash | {}", line);
        for (const auto& line : r.crash_returns) log.infof(L"crash stack | {}", line);
    };

    for (;;) {
        DEBUG_EVENT ev{};
        // Once we've killed the target, keep pumping (with a bound) so the debug
        // session tears down cleanly and the launcher can exit promptly.
        if (!::WaitForDebugEvent(&ev, terminating ? 5000 : INFINITE)) {
            if (!terminating)
                log.warnf(L"WaitForDebugEvent failed: {}", win32::last_error());
            break;
        }
        DWORD cont = DBG_CONTINUE;

        switch (ev.dwDebugEventCode) {
            case CREATE_THREAD_DEBUG_EVENT: {
                threads[ev.dwThreadId] = ev.u.CreateThread.hThread;
                break;
            }
            case EXIT_THREAD_DEBUG_EVENT: {
                threads.erase(ev.dwThreadId);
                break;
            }
            case CREATE_PROCESS_DEBUG_EVENT: {
                threads[ev.dwThreadId] = ev.u.CreateProcessInfo.hThread;
                record_module(r.modules, process,
                              reinterpret_cast<uintptr_t>(ev.u.CreateProcessInfo.lpBaseOfImage));
                if (ev.u.CreateProcessInfo.hFile) ::CloseHandle(ev.u.CreateProcessInfo.hFile);
                break;
            }
            case LOAD_DLL_DEBUG_EVENT: {
                auto base = reinterpret_cast<uintptr_t>(ev.u.LoadDll.lpBaseOfDll);
                record_module(r.modules, process, base);
                // the record for this base: appended, or one replaced in place
                const auto& m = *std::find_if(r.modules.begin(), r.modules.end(),
                                              [base](const ModuleRec& x) { return x.base == base; });
                if (m.name.size() >= 8) {
                    std::wstring lower = m.name;
                    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
                    if (lower.find(L"samp.dll") != std::wstring::npos) r.samp_loaded = true;
                }
                log.tracef(L"module loaded : {} @ {:#010x}", m.name, base);
                if (ev.u.LoadDll.hFile) ::CloseHandle(ev.u.LoadDll.hFile);
                break;
            }
            case UNLOAD_DLL_DEBUG_EVENT: {
                forget_module(r.modules, reinterpret_cast<uintptr_t>(ev.u.UnloadDll.lpBaseOfDll));
                break;
            }
            case OUTPUT_DEBUG_STRING_EVENT: {
                std::wstring s = read_debug_string(process, ev.u.DebugString);
                if (!s.empty()) {
                    // The trace log always gets the line; the report's ring buffer
                    // only gets it if it isn't a plugin echoing its own log file.
                    log.tracef(L"OutputDebugString: {}", s);
                    if (!keep_all_output && is_mirrored_log_line(s)) {
                        ++r.debug_strings_omitted;
                        break;
                    }
                    r.debug_strings.push_back(std::move(s));
                    if (r.debug_strings.size() > kMaxDebugStrings)
                        r.debug_strings.erase(r.debug_strings.begin());
                    if (fault_context_taken) ++r.lines_after_fault;
                }
                break;
            }
            case EXCEPTION_DEBUG_EVENT: {
                const auto& er = ev.u.Exception.ExceptionRecord;
                const DWORD ec = er.ExceptionCode;
                const bool first = ev.u.Exception.dwFirstChance != 0;

                // Already killing the target - swallow anything still in flight.
                if (terminating) { cont = DBG_CONTINUE; break; }

                // Breakpoints/single-steps (attach BP, WOW64 BP, DebugBreak) aren't
                // crashes - swallow them.
                if (ec == EXCEPTION_BREAKPOINT || ec == 0x4000001F /*WX86 BP*/ ||
                    ec == EXCEPTION_SINGLE_STEP) {
                    // ...but one in the game's own code is worth a line: the
                    // attach break is ntdll's (DbgUiRemoteBreakin).
                    if (ec != EXCEPTION_SINGLE_STEP) {
                        ExceptionRec x;
                        x.code = ec;
                        x.address = reinterpret_cast<uintptr_t>(er.ExceptionAddress);
                        locate(r.modules, x);
                        std::wstring lower = x.module;
                        std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
                        if (lower != L"ntdll.dll" && lower != L"kernelbase.dll" && lower != L"kernel32.dll") {
                            auto t = threads.find(ev.dwThreadId);
                            std::wstring line = describe_break(process, t == threads.end() ? nullptr : t->second, x);
                            log.warnf(L"debug break: {}", line);
                            r.breaks.push_back(std::move(line));
                            if (r.breaks.size() > kMaxBreaks) r.breaks.erase(r.breaks.begin());
                        }
                    }
                    cont = DBG_CONTINUE;
                    break;
                }

                // A bad handle given to CloseHandle and the like: Windows and Wine
                // raise these only while a debugger is attached; without one the
                // call just fails. Carry on as it would. Handing one to the game
                // as a crash sends it down the crash path, and during the DLL
                // teardown at exit that hangs the game (CLEO 4's
                // IntOperations.cleo closes a dead handle there).
                if (ec == 0xC0000008 /*INVALID_HANDLE*/ || ec == 0xC0000235 /*HANDLE_NOT_CLOSABLE*/) {
                    ExceptionRec x;
                    x.code = ec;
                    x.address = reinterpret_cast<uintptr_t>(er.ExceptionAddress);
                    locate(r.modules, x);
                    log.debugf(L"{:#010x} ({}) at {}+{:#x}: raised only for a debugger, ignored",
                               ec, decode_status(ec), x.module, x.module_offset);
                    cont = DBG_CONTINUE;
                    break;
                }

                // Hard faults are NTSTATUS 0xC0000xxx (access violation, illegal
                // instruction, ...). Benign first-chance exceptions - C++/CLR EH
                // (0xE.......), thread-name (0x406D1388), guard-page - are passed
                // straight through so the app handles them normally.
                const bool hard = (ec & 0xF0000000u) == 0xC0000000u;
                if (!hard) { cont = DBG_EXCEPTION_NOT_HANDLED; break; }

                // A first touch of reserved memory: the game's own handler commits
                // the page (the limit adjuster's unlimited arrays). Unhandled ones
                // come back as last chance.
                if (first && ec == EXCEPTION_ACCESS_VIOLATION && er.NumberParameters >= 2) {
                    MEMORY_BASIC_INFORMATION mbi{};
                    const auto target = reinterpret_cast<LPCVOID>(er.ExceptionInformation[1]);
                    if (::VirtualQueryEx(process, target, &mbi, sizeof mbi) && mbi.State == MEM_RESERVE) {
                        static unsigned reserved_faults;
                        if (++reserved_faults <= 3) {
                            ExceptionRec x;
                            x.code = ec;
                            x.address = reinterpret_cast<uintptr_t>(er.ExceptionAddress);
                            locate(r.modules, x);
                            log.debugf(L"first touch of reserved memory at {:#010x} from {}+{:#x}: "
                                       L"passed to the game, which commits it",
                                       reinterpret_cast<uintptr_t>(target), x.module, x.module_offset);
                        }
                        cont = DBG_EXCEPTION_NOT_HANDLED;
                        break;
                    }
                }

                const uintptr_t addr = reinterpret_cast<uintptr_t>(er.ExceptionAddress);
                if (first && ec == last_fault_code && addr == last_fault_addr)
                    ++fault_repeats;
                else { fault_repeats = 1; last_fault_code = ec; last_fault_addr = addr; }

                // Name things by the loader's list as it is NOW (see refresh_modules);
                // a __fastfail arrives last-chance without a first.
                if (fault_repeats == 1) refresh_modules(r.modules, process);

                ExceptionRec x;
                x.code = ec; x.address = addr; x.first_chance = first;
                locate(r.modules, x);
                record_access(r.modules, er, x);
                const std::wstring access = describe_access(x);
                const std::wstring access_suffix =
                    access.empty() ? std::wstring{} : L" - " + access;

                // Freeze what the game had said up to this point, before the
                // shutdown chatter that follows a crash pushes it out.
                if (!fault_context_taken) {
                    fault_context_taken = true;
                    r.fault_context = r.debug_strings;
                }

                // The first hard fault's first chance: the thread is stopped ON
                // the faulting instruction, so this is the one moment its
                // registers are the fault's. Read them (SA-MP's crash text), the
                // stack's code addresses, and write the dump - once a session.
                if (first && fault_repeats == 1 && !crash_captured) {
                    capture_crash(ev, er);

                    // Hand the fault to the game: detach with the exception
                    // event still pending, so it is dispatched as if no
                    // debugger had been there - the game's own handlers, then
                    // its unhandled-exception filter (SA-MP's crash dialog),
                    // which Windows and Wine both skip while a debugger is
                    // attached. A fault the game handles just carries on,
                    // unwatched; the exit is still reported.
                    if (opts.crash_dialog) {
                        log.debugf(L"first-chance {:#010x} ({}) at {}+{:#x}{} - handing it to the game",
                                   ec, decode_status(ec), x.module, x.module_offset, access_suffix);
                        r.exceptions.push_back(x);
                        if (::DebugActiveProcessStop(pid)) {
                            r.handed_over = true;
                            log.infof(L"detached at the fault: the game's own crash handler "
                                      L"(SA-MP's dialog) can run - waiting for the game to end");
                            ::WaitForSingleObject(process, INFINITE);
                            DWORD code = 0;
                            ::GetExitCodeProcess(process, &code);
                            r.process_exited = true;
                            r.exit_code = code;
                            r.run_ms = ::GetTickCount64() - launch_tick;
                            r.lines_after_fault = 0;
                            log.infof(L"process exited after {} ms with status {} ({})",
                                      r.run_ms, r.exit_code, decode_status(r.exit_code));
                            return r;
                        }
                        log.warnf(L"could not detach ({}): staying attached, so SA-MP's crash "
                                  L"dialog will not show", win32::last_error());
                        r.exceptions.pop_back();
                    }
                }

                // Fatal when the app didn't handle it: it arrived as last-chance,
                // or the same first-chance fault looped back to us unresolved.
                if (!first || fault_repeats >= 2) {
                    // __fastfail (int 29h: abort(), a failed /GS cookie, ...)
                    // reaches a debugger as last-chance only, never first: its
                    // thread is stopped on it now, so capture it here.
                    if (!crash_captured)
                        capture_crash(ev, er);
                    std::wstring fast_fail;
                    if (ec == 0xC0000409 && er.NumberParameters >= 1)
                        fast_fail = std::format(L" - fast fail code {}{}", er.ExceptionInformation[0],
                                                describe_fast_fail(er.ExceptionInformation[0]));
                    log.errorf(L"unhandled {:#010x} ({}) at {}+{:#x}{}{} - terminating target",
                               ec, decode_status(ec), x.module, x.module_offset,
                               access_suffix, fast_fail);
                    // Re-anchor the output snapshot on the fault that actually
                    // killed the game, not on some earlier one the app handled.
                    r.fault_context = r.debug_strings;
                    r.lines_after_fault = 0;
                    r.exceptions.push_back(std::move(x));
                    ::TerminateProcess(process, ec);
                    terminating = true;
                    r.process_exited = true;
                    r.exit_code = ec;
                    r.run_ms = ::GetTickCount64() - launch_tick;
                    // Don't return yet - fall through to drain to EXIT_PROCESS.
                    cont = DBG_CONTINUE;
                    break;
                }

                // First sighting: record it, then give the app's own handler
                // (e.g. crashes.asi) a chance before we decide it's fatal.
                log.debugf(L"first-chance {:#010x} ({}) at {}+{:#x}{}",
                           ec, decode_status(ec), x.module, x.module_offset,
                           access_suffix);
                r.exceptions.push_back(std::move(x));
                if (r.exceptions.size() > kMaxExceptions)
                    r.exceptions.erase(r.exceptions.begin());
                cont = DBG_EXCEPTION_NOT_HANDLED;
                break;
            }
            case EXIT_PROCESS_DEBUG_EVENT: {
                r.process_exited = true;
                r.exit_code = ev.u.ExitProcess.dwExitCode;
                r.run_ms = ::GetTickCount64() - launch_tick;
                log.infof(L"process exited after {} ms with status {} ({})",
                          r.run_ms, r.exit_code, decode_status(r.exit_code));
                ::ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, DBG_CONTINUE);
                return r;
            }
            default:
                break;
        }
        ::ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, cont);
    }

    if (!r.run_ms) r.run_ms = ::GetTickCount64() - launch_tick;
    return r;
}

std::wstring format_report(const ReportContext& ctx, const Result& r, bool compact) {
    const bool crashed = r.process_exited && is_crash_status(r.exit_code);

    std::wstring outcome;
    if (!r.process_exited)
        outcome = std::format(L"STILL RUNNING after {} ms (healthy)", r.run_ms);
    else if (crashed)
        outcome = std::format(L"CRASHED after {} ms", r.run_ms);
    else if (r.exit_code == 0)
        outcome = std::format(L"exited cleanly after {} ms", r.run_ms);
    else
        outcome = std::format(L"exited abnormally after {} ms", r.run_ms);

    std::wstring b;
    b += L"===== SAMPCMD REPORT (copy everything between the lines) =====\n";
    b += std::format(L"sampcmd     : {}\n", ctx.version);
    b += std::format(L"os / wine   : {} | {}\n", ctx.os, ctx.wine);
    b += std::format(L"target      : {}\n", ctx.target);
    b += std::format(L"outcome     : {}\n", outcome);
    if (r.process_exited)
        b += std::format(L"exit/status : 0x{:08X}  {}\n", r.exit_code, decode_status(r.exit_code));

    // The most telling exception: prefer a last-chance one, else the latest.
    if (!r.exceptions.empty()) {
        const ExceptionRec* pick = &r.exceptions.back();
        for (auto it = r.exceptions.rbegin(); it != r.exceptions.rend(); ++it)
            if (!it->first_chance) { pick = &*it; break; }
        b += std::format(L"fault       : 0x{:08X} {} at {}+0x{:X}\n",
                         pick->code, decode_status(pick->code), pick->module,
                         pick->module_offset);
        if (const std::wstring access = describe_access(*pick); !access.empty())
            b += std::format(L"accessing   : {}\n", access);
        if (r.exceptions.size() > 1)
            b += std::format(L"exceptions  : {} seen (showing the fatal one)\n",
                             r.exceptions.size());
    }

    for (const auto& line : r.breaks)
        b += std::format(L"debug break : {}\n", line);

    // A crash handler picks its own exit code, so the exit status above is
    // what the game ended with after sampcmd let go - not a verdict.
    if (r.handed_over)
        b += L"handed over : sampcmd let go at the fault so the game's own crash handler "
             L"(SA-MP's dialog) could run\n";
    if (!r.dump_path.empty())
        b += std::format(L"dump        : {} ({:.1f} MB)\n", r.dump_path, r.dump_bytes / 1048576.0);
    else if (!r.dump_error.empty())
        b += std::format(L"dump        : none - {}\n", r.dump_error);
    if (!r.crash_returns.empty()) {
        b += L"code on the stack (nearest the fault first):\n";
        for (const auto& line : r.crash_returns) b += std::format(L"  {}\n", line);
    }
    if (!r.crash_text.empty()) {
        // What SA-MP's own crash dialog shows, in its format. The message box
        // gets the head of it; the 40-line stack dump is the log's.
        b += L"SA-MP crash info:\n";
        for (const auto& line : r.crash_text) {
            if (compact && line.starts_with(L"+")) continue;
            b += std::format(L"  {}\n", line);
        }
    }

    if (r.attached)
        b += std::format(L"modules     : {} loaded | samp.dll {}\n",
                         r.modules.size(), r.samp_loaded ? L"LOADED" : L"NOT loaded");

    // Prefer the output as it stood at the fault - the tail of the ring buffer is
    // usually just teardown noise from whatever ran after the crash.
    if (!r.fault_context.empty()) {
        b += L"output@fault:\n";
        for (const auto& s : r.fault_context)
            b += std::format(L"  > {}\n", s);
        const size_t tail = std::min({r.lines_after_fault, r.debug_strings.size(),
                                      kMaxTailAfterFault});
        if (tail) {
            b += L"  ... then:\n";
            for (size_t i = r.debug_strings.size() - tail; i < r.debug_strings.size(); ++i)
                b += std::format(L"  > {}\n", r.debug_strings[i]);
        }
    } else if (!r.debug_strings.empty()) {
        b += L"last output :\n";
        for (const auto& s : r.debug_strings)
            b += std::format(L"  > {}\n", s);
    }
    if (r.debug_strings_omitted)
        b += std::format(L"omitted     : {} mirrored plugin log line(s), e.g. cleo.asi "
                         L"(--all-output keeps them)\n", r.debug_strings_omitted);

    b += std::format(L"log file    : {}\n", ctx.log_path);
    b += L"===== END REPORT =====";
    return b;
}

} // namespace monitor
