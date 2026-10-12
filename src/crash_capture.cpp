#include "windows_prelude.hpp"
#include "crash_capture.hpp"

#include <dbghelp.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <format>

namespace crash {
namespace {

// samp.dll 0.3.7-R5 (PE timestamp 0x6372C39E). The handler at samp.dll+0x605B0
// prints these globals; the offsets are this build's, so they are read only
// when the loaded samp.dll is this build.
constexpr DWORD     kR5Timestamp   = 0x6372C39E;
constexpr uintptr_t kR5ScmOp       = 0x26B55C;   // DWORD, "SCM Op: 0x%X"
constexpr uintptr_t kR5LDbg        = 0x26B560;   // int,   "lDbg: %d"
constexpr uintptr_t kR5LastRendObj = 0x1516EC;   // WORD,  "LastRendObj: %u"
constexpr uintptr_t kR5GameVersion = 0x26EB28;   // int: 1 US 1.0, 2 EU 1.0
constexpr size_t    kStackBytes    = 0x280;      // what the handler dumps from ESP
constexpr size_t    kMaxReturns    = 24;

template <typename T>
bool read(HANDLE process, uintptr_t addr, T& out) {
    SIZE_T got = 0;
    return ::ReadProcessMemory(process, reinterpret_cast<LPCVOID>(addr), &out, sizeof(T), &got) &&
           got == sizeof(T);
}

std::wstring lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), ::towlower);
    return s;
}

const monitor::ModuleRec* find_module(const std::vector<monitor::ModuleRec>& mods, const wchar_t* name) {
    for (const auto& m : mods) {
        const std::wstring n = lower(m.name);
        if (n == name || (n.size() > wcslen(name) && n.ends_with(std::wstring(L"\\") + name)))
            return &m;
    }
    return nullptr;
}

// A module's size: as recorded, else its PE header's SizeOfImage. The record
// is taken at the load event, when GetModuleInformation often has nothing yet
// (Wine), so many modules arrive here with size 0.
uintptr_t image_size(HANDLE process, const monitor::ModuleRec& m) {
    if (m.size) return m.size;
    LONG lfanew = 0;
    DWORD size = 0;
    if (read(process, m.base + 0x3C, lfanew) &&
        read(process, m.base + static_cast<uintptr_t>(lfanew) + 24 + 56, size))   // OptionalHeader32.SizeOfImage
        return size;
    return 0;
}

const monitor::ModuleRec* module_at(HANDLE process, const std::vector<monitor::ModuleRec>& mods,
                                    uintptr_t addr) {
    for (const auto& m : mods) {
        if (addr < m.base) continue;
        const uintptr_t size = image_size(process, m);
        if (size && addr < m.base + size) return &m;
    }
    return nullptr;
}

std::wstring base_name(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

// A stack word that lands in a module's executable section is a candidate
// return address. Reading the section table is what keeps data pointers into
// a module (its .data, its .rdata strings) out of the list.
bool in_code(HANDLE process, const monitor::ModuleRec& m, uintptr_t addr) {
    LONG lfanew = 0;
    if (!read(process, m.base + 0x3C, lfanew)) return false;
    const uintptr_t nt = m.base + static_cast<uintptr_t>(lfanew);
    WORD nsec = 0, optsize = 0;
    if (!read(process, nt + 6, nsec) || !read(process, nt + 20, optsize)) return false;
    uintptr_t sec = nt + 24 + optsize;
    const uintptr_t rva = addr - m.base;
    for (WORD i = 0; i < nsec && i < 96; ++i, sec += 40) {
        DWORD vsize = 0, va = 0, chars = 0;
        if (!read(process, sec + 8, vsize) || !read(process, sec + 12, va) ||
            !read(process, sec + 36, chars)) return false;
        if (rva >= va && rva < va + vsize) return (chars & IMAGE_SCN_MEM_EXECUTE) != 0;
    }
    return false;
}

std::wstring now_stamp() {
    SYSTEMTIME t{};
    ::GetLocalTime(&t);
    return std::format(L"{:04}{:02}{:02}-{:02}{:02}{:02}", t.wYear, t.wMonth, t.wDay,
                       t.wHour, t.wMinute, t.wSecond);
}

using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                          PMINIDUMP_EXCEPTION_INFORMATION,
                                          PMINIDUMP_USER_STREAM_INFORMATION,
                                          PMINIDUMP_CALLBACK_INFORMATION);

void write_dump(Capture& out, HANDLE process, DWORD pid, DWORD tid, const EXCEPTION_RECORD& er,
                CONTEXT* ctx, DumpKind kind, const std::wstring& dir) {
    HMODULE dbghelp = ::LoadLibraryW(L"dbghelp.dll");
    auto write = dbghelp ? reinterpret_cast<MiniDumpWriteDumpFn>(
                               reinterpret_cast<void*>(::GetProcAddress(dbghelp, "MiniDumpWriteDump")))
                         : nullptr;
    if (!write) { out.dump_error = L"dbghelp.dll has no MiniDumpWriteDump"; return; }

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::wstring path = (std::filesystem::path(dir) /
                               (L"sampcmd-crash-" + now_stamp() + L".dmp")).wstring();
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        out.dump_error = std::format(L"cannot create {} (error {})", path, ::GetLastError());
        return;
    }

    // Mini: every thread's stack and registers, loaded and unloaded modules,
    // handles, the modules' data sections and the memory the stacks point at -
    // a few MB, enough to read the objects around the fault. Full: everything.
    const auto type = static_cast<MINIDUMP_TYPE>(
        kind == DumpKind::full
            ? (MiniDumpWithFullMemory | MiniDumpWithHandleData | MiniDumpWithUnloadedModules |
               MiniDumpWithThreadInfo | MiniDumpWithFullMemoryInfo)
            : (MiniDumpWithDataSegs | MiniDumpWithHandleData | MiniDumpWithUnloadedModules |
               MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo |
               MiniDumpWithProcessThreadData));

    // The exception record and context live in OUR address space
    // (ClientPointers = FALSE): the record came with the debug event, the
    // context from GetThreadContext.
    EXCEPTION_RECORD record = er;
    EXCEPTION_POINTERS pointers{&record, ctx};
    MINIDUMP_EXCEPTION_INFORMATION mei{tid, &pointers, FALSE};
    const BOOL ok = write(process, pid, file, type, ctx ? &mei : nullptr, nullptr, nullptr);
    const DWORD err = ok ? 0 : ::GetLastError();
    LARGE_INTEGER size{};
    ::GetFileSizeEx(file, &size);
    ::CloseHandle(file);
    if (!ok) {
        ::DeleteFileW(path.c_str());
        out.dump_error = std::format(L"MiniDumpWriteDump failed (error {:#x})", err);
        return;
    }
    out.dump_path = path;
    out.dump_bytes = static_cast<uint64_t>(size.QuadPart);
}

} // namespace

void prune_dumps(const std::wstring& dir, size_t keep) {
    std::error_code ec;
    std::vector<std::filesystem::path> dumps;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        const std::wstring n = e.path().filename().wstring();
        if (n.starts_with(L"sampcmd-crash-") && n.ends_with(L".dmp")) dumps.push_back(e.path());
    }
    if (dumps.size() <= keep) return;
    std::sort(dumps.begin(), dumps.end());   // the name sorts by time
    for (size_t i = 0; i + keep < dumps.size(); ++i) std::filesystem::remove(dumps[i], ec);
}

Capture capture(HANDLE process, DWORD pid, HANDLE thread, DWORD tid,
                const EXCEPTION_RECORD& er, const std::vector<monitor::ModuleRec>& modules,
                DumpKind kind, const std::wstring& dump_dir) {
    Capture out;
    out.taken = true;

    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_ALL;
    const bool have_ctx = thread && ::GetThreadContext(thread, &ctx);

    // ---- SA-MP's crash text (samp.dll's own format strings) ----
    const monitor::ModuleRec* samp = find_module(modules, L"samp.dll");
    bool r5 = false;
    if (samp) {
        LONG lfanew = 0;
        DWORD stamp = 0;
        r5 = read(process, samp->base + 0x3C, lfanew) &&
             read(process, samp->base + static_cast<uintptr_t>(lfanew) + 8, stamp) &&
             stamp == kR5Timestamp;
    }
    auto& L = out.samp_lines;
    L.push_back(std::format(L"SA-MP {}", r5 ? L"0.3.7-R5" : (samp ? L"(not 0.3.7-R5)" : L"(samp.dll not loaded)")));
    L.push_back(std::format(L"Exception At Address: 0x{:08X}",
                            reinterpret_cast<uintptr_t>(er.ExceptionAddress)));
    L.push_back(std::format(L"Base: 0x{:08X}", samp ? samp->base : 0));
    L.push_back(L"");
    if (have_ctx) {
        L.push_back(L"Registers:");
        L.push_back(std::format(L"EAX: 0x{:08X}\tEBX: 0x{:08X}\tECX: 0x{:08X}\tEDX: 0x{:08X}",
                                ctx.Eax, ctx.Ebx, ctx.Ecx, ctx.Edx));
        L.push_back(std::format(L"ESI: 0x{:08X}\tEDI: 0x{:08X}\tEBP: 0x{:08X}\tESP: 0x{:08X}",
                                ctx.Esi, ctx.Edi, ctx.Ebp, ctx.Esp));
        L.push_back(std::format(L"EFLAGS: 0x{:08X}", ctx.EFlags));
        L.push_back(L"");
        L.push_back(L"Stack:");
        for (size_t off = 0; off < kStackBytes; off += 16) {
            std::wstring line = std::format(L"+{:04X}:", off);
            for (size_t i = 0; i < 16; i += 4) {
                DWORD v = 0;
                line += read(process, ctx.Esp + off + i, v) ? std::format(L" 0x{:08X}  ", v)
                                                           : std::wstring(L" ??????????  ");
            }
            while (!line.empty() && line.back() == L' ') line.pop_back();
            L.push_back(std::move(line));
        }
        // the candidates for "who called this", nearest the fault first
        for (size_t off = 0; off < kStackBytes && out.return_addrs.size() < kMaxReturns; off += 4) {
            DWORD v = 0;
            if (!read(process, ctx.Esp + off, v)) continue;
            const monitor::ModuleRec* m = module_at(process, modules, v);
            if (m && in_code(process, *m, v))
                out.return_addrs.push_back(std::format(L"+{:04X}  {}+0x{:X}", off,
                                                       base_name(m->name), v - m->base));
        }
    } else {
        L.push_back(std::format(L"Registers: unavailable (GetThreadContext failed, error {})",
                                ::GetLastError()));
    }
    if (r5) {
        DWORD scm = 0; int ldbg = 0; WORD rend = 0; int ver = 0;
        read(process, samp->base + kR5ScmOp, scm);
        read(process, samp->base + kR5LDbg, ldbg);
        read(process, samp->base + kR5LastRendObj, rend);
        read(process, samp->base + kR5GameVersion, ver);
        L.push_back(L"");
        L.push_back(std::format(L"SCM Op: 0x{:X}, lDbg: {} LastRendObj: {}", scm, ldbg, rend));
        L.push_back(L"");
        L.push_back(std::format(L"Game Version: {}",
                                ver == 1 ? L"US 1.0" : ver == 2 ? L"EU 1.0" : L"UNKNOWN"));
    }

    // ---- the dump ----
    if (kind != DumpKind::none) {
        write_dump(out, process, pid, tid, er, have_ctx ? &ctx : nullptr, kind, dump_dir);
        // full dumps are the size of the game's memory: keep fewer of them
        if (!out.dump_path.empty()) prune_dumps(dump_dir, kind == DumpKind::full ? 2 : 5);
    }
    return out;
}

} // namespace crash
