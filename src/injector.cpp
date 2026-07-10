#include "injector.hpp"
#include "error_formatter.hpp"
#include "logger.hpp"
#include "win32_helpers.hpp"

#include <cstdint>
#include <format>

namespace fs = std::filesystem;

InjectionResult Injector::inject(HANDLE process, DWORD pid, const fs::path& dll_path) {
    InjectionResult r;
    log_.section(L"Injection");

    const std::wstring dll = dll_path.wstring();
    log_.infof(L"target pid     : {}", pid);
    log_.infof(L"dll to inject  : {}", dll);

    // --- Resolve LoadLibraryW in the target ---------------------------------
    // kernel32 is mapped at the same base in every process of the session, so the
    // address we resolve locally is valid in the remote process too.
    HMODULE kernel32 = ::GetModuleHandleW(L"kernel32.dll");
    auto load_library = kernel32
        ? reinterpret_cast<LPTHREAD_START_ROUTINE>(
              reinterpret_cast<void*>(::GetProcAddress(kernel32, "LoadLibraryW")))
        : nullptr;

    if (!load_library) {
        r.failed_stage = L"GetProcAddress(LoadLibraryW)";
        r.last_error   = ::GetLastError();
        err_.report_win32(r.failed_stage, r.last_error, {
            {L"Detail", L"Could not locate LoadLibraryW in kernel32.dll"},
        });
        return r;
    }
    log_.debugf(L"LoadLibraryW   : {:#018x}", reinterpret_cast<uintptr_t>(load_library));

    // --- Allocate space for the DLL path in the target ----------------------
    const SIZE_T bytes = (dll.size() + 1) * sizeof(wchar_t);  // include NUL
    void* remote_raw = ::VirtualAllocEx(process, nullptr, bytes,
                                        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote_raw) {
        r.failed_stage = L"VirtualAllocEx";
        r.last_error   = ::GetLastError();
        err_.report_win32(r.failed_stage, r.last_error, {
            {L"Bytes", std::format(L"{}", bytes)},
            {L"PID",   std::format(L"{}", pid)},
        });
        return r;
    }
    win32::RemoteMemory remote_path(process, remote_raw);  // RAII: VirtualFreeEx
    log_.debugf(L"remote alloc   : {:#018x} ({} bytes, PAGE_READWRITE)",
                reinterpret_cast<uintptr_t>(remote_raw), bytes);

    // --- Write the path string ----------------------------------------------
    SIZE_T written = 0;
    if (!::WriteProcessMemory(process, remote_raw, dll.c_str(), bytes, &written) ||
        written != bytes) {
        r.failed_stage = L"WriteProcessMemory";
        r.last_error   = ::GetLastError();
        err_.report_win32(r.failed_stage, r.last_error, {
            {L"Expected", std::format(L"{} bytes", bytes)},
            {L"Written",  std::format(L"{} bytes", written)},
        });
        return r;
    }
    log_.debugf(L"wrote path     : {} of {} bytes", written, bytes);

    // --- Create the remote thread that calls LoadLibraryW(path) -------------
    DWORD remote_tid = 0;
    HANDLE thread_raw = ::CreateRemoteThread(process, nullptr, 0, load_library,
                                             remote_raw, 0, &remote_tid);
    if (!thread_raw) {
        r.failed_stage = L"CreateRemoteThread";
        r.last_error   = ::GetLastError();
        err_.report_win32(r.failed_stage, r.last_error, {
            {L"PID", std::format(L"{}", pid)},
        });
        return r;
    }
    win32::ScopedHandle thread(thread_raw);
    log_.debugf(L"remote thread  : tid {}", remote_tid);

    // --- Wait for LoadLibraryW to finish, with a timeout --------------------
    // INFINITE risks hanging the launcher if the target wedges; 30s is generous
    // for a DLL that only sets up hooks in DllMain.
    const DWORD wait = ::WaitForSingleObject(thread.get(), 30000);
    if (wait == WAIT_TIMEOUT) {
        r.failed_stage = L"WaitForSingleObject(timeout)";
        r.last_error   = 0;
        err_.report(L"Injection timed out",
                    L"The remote LoadLibraryW thread did not finish within 30s.",
                    {{L"DLL", dll}, {L"PID", std::format(L"{}", pid)}});
        return r;
    }
    if (wait != WAIT_OBJECT_0) {
        r.failed_stage = L"WaitForSingleObject";
        r.last_error   = ::GetLastError();
        err_.report_win32(r.failed_stage, r.last_error, {
            {L"WaitResult", std::format(L"0x{:08X}", wait)},
        });
        return r;
    }

    // --- Read back the result: exit code == HMODULE from LoadLibraryW -------
    DWORD exit_code = 0;
    if (!::GetExitCodeThread(thread.get(), &exit_code)) {
        r.failed_stage = L"GetExitCodeThread";
        r.last_error   = ::GetLastError();
        err_.report_win32(r.failed_stage, r.last_error, {});
        return r;
    }
    r.thread_exit   = exit_code;
    r.remote_module = reinterpret_cast<void*>(static_cast<uintptr_t>(exit_code));

    if (exit_code == 0) {
        // This is the exact failure the rewrite exists to catch: LoadLibraryW
        // returned NULL inside gta_sa.exe. The launcher must not pretend success.
        r.failed_stage = L"LoadLibraryW (in target)";
        r.last_error   = 0;
        err_.report(
            L"DLL injection failed",
            L"LoadLibraryW returned NULL inside gta_sa.exe (the DLL did not load).",
            {
                {L"DLL", dll},
                {L"PID", std::format(L"{}", pid)},
                {L"Likely causes",
                 L"missing dependency of samp.dll, wrong architecture, or a "
                 L"failed DllMain. Check that samp.dll's dependencies resolve "
                 L"from the game directory."},
            });
        return r;
    }

    log_.infof(L"LoadLibraryW OK: {} loaded at HMODULE {:#010x} in target",
               dll_path.filename().wstring(), exit_code);
    r.success = true;
    return r;
}
