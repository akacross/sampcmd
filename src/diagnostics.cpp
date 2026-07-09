#include "windows_prelude.hpp"
#include "diagnostics.hpp"
#include "logger.hpp"

#include <format>
#include <iterator>
#include <string>

namespace fs = std::filesystem;

namespace {

using RtlGetVersion_t = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);

std::wstring env(const wchar_t* name) {
    DWORD n = ::GetEnvironmentVariableW(name, nullptr, 0);
    if (n == 0) return {};
    std::wstring buf(n, L'\0');
    DWORD got = ::GetEnvironmentVariableW(name, buf.data(), n);
    buf.resize(got);
    return buf;
}

} // namespace

namespace diagnostics {

fs::path self_executable_path() {
    // MAX_PATH is not enough for long paths; grow until it fits.
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        DWORD n = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return {};
        if (n < buf.size()) { buf.resize(n); break; }
        buf.resize(buf.size() * 2);
    }
    return fs::path(buf);
}

fs::path self_directory() {
    auto p = self_executable_path();
    return p.empty() ? fs::path{} : p.parent_path();
}

std::wstring os_version_string() {
    RTL_OSVERSIONINFOW vi{};
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll")) {
        if (auto rtl = reinterpret_cast<RtlGetVersion_t>(
                reinterpret_cast<void*>(::GetProcAddress(ntdll, "RtlGetVersion")))) {
            if (rtl(&vi) == 0) {
                return std::format(L"{}.{}.{}", vi.dwMajorVersion, vi.dwMinorVersion,
                                   vi.dwBuildNumber);
            }
        }
    }
    return L"<unknown>";
}

std::wstring process_arch_string() {
#if defined(_WIN64)
    std::wstring arch = L"x64 (64-bit)";
#else
    std::wstring arch = L"x86 (32-bit)";
#endif
    BOOL wow64 = FALSE;
    if (::IsWow64Process(::GetCurrentProcess(), &wow64) && wow64)
        arch += L" [WOW64]";
    return arch;
}

void log_snapshot(Logger& log) {
    log.section(L"Diagnostics");
    log.infof(L"launcher exe   : {}", self_executable_path().wstring());
    log.infof(L"launcher dir   : {}", self_directory().wstring());

    wchar_t cwd[MAX_PATH * 2];
    if (::GetCurrentDirectoryW(static_cast<DWORD>(std::size(cwd)), cwd))
        log.infof(L"working dir    : {}", cwd);

    log.infof(L"process arch   : {}", process_arch_string());
    log.infof(L"OS version     : {}", os_version_string());
    log.infof(L"raw cmdline    : {}", ::GetCommandLineW());

    // A few environment variables that meaningfully change GTA/SA:MP behavior.
    for (const wchar_t* var : {L"USERPROFILE", L"WINEPREFIX", L"STEAM_COMPAT_DATA_PATH",
                               L"__GL_THREADED_OPTIMIZATIONS", L"DXVK_HUD"}) {
        std::wstring v = env(var);
        if (!v.empty()) log.infof(L"env {:<12}: {}", var, v);
    }
}

} // namespace diagnostics
