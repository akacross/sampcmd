#include "process_launcher.hpp"
#include "error_formatter.hpp"
#include "logger.hpp"
#include "win32_helpers.hpp"

#include <format>
#include <vector>

namespace fs = std::filesystem;

std::optional<LaunchedProcess> ProcessLauncher::create_suspended(
    const fs::path& exe_path, const std::wstring& command_line, const fs::path& work_dir) {

    log_.section(L"Launch");
    log_.infof(L"exe            : {}", exe_path.wstring());
    log_.infof(L"working dir    : {}", work_dir.wstring());
    log_.infof(L"command line   : {}", command_line);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);   // Wine is stricter than Windows about this.

    PROCESS_INFORMATION pi{};

    // CreateProcessW may write to lpCommandLine, so hand it a mutable buffer.
    std::vector<wchar_t> mutable_cmd(command_line.begin(), command_line.end());
    mutable_cmd.push_back(L'\0');

    const DWORD flags = CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT;

    const std::wstring exe_str = exe_path.wstring();
    const std::wstring dir_str = work_dir.wstring();

    log_.debugf(L"CreateProcessW flags: 0x{:08X} (CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT)",
                flags);

    BOOL ok = ::CreateProcessW(
        exe_str.c_str(),          // lpApplicationName (explicit, avoids search rules)
        mutable_cmd.data(),       // lpCommandLine     (mutable)
        nullptr, nullptr,         // process/thread security
        FALSE,                    // inherit handles
        flags,
        nullptr,                  // environment (inherit)
        dir_str.empty() ? nullptr : dir_str.c_str(),  // <-- working directory
        &startup, &pi);

    if (!ok) {
        const DWORD e = ::GetLastError();
        err_.report_win32(L"CreateProcess", e, {
            {L"GTA", exe_str},
            {L"Working Dir", dir_str},
            {L"Command Line", command_line},
        });
        return std::nullopt;
    }

    LaunchedProcess out;
    out.process = win32::ScopedHandle(pi.hProcess);
    out.thread  = win32::ScopedHandle(pi.hThread);
    out.pid     = pi.dwProcessId;
    out.tid     = pi.dwThreadId;

    log_.infof(L"created pid    : {} (tid {}) [suspended]", out.pid, out.tid);
    return out;
}

bool ProcessLauncher::resume(LaunchedProcess& p) {
    log_.infof(L"resuming main thread of pid {}", p.pid);
    DWORD prev = ::ResumeThread(p.thread.get());
    if (prev == static_cast<DWORD>(-1)) {
        const DWORD e = ::GetLastError();
        err_.report_win32(L"ResumeThread", e, {
            {L"PID", std::format(L"{}", p.pid)},
        });
        return false;
    }
    log_.debugf(L"ResumeThread previous suspend count: {}", prev);
    return true;
}
