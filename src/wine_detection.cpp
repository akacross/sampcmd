#include "windows_prelude.hpp"
#include "wine_detection.hpp"
#include "logger.hpp"
#include "strconv.hpp"

#include <format>
#include <string>
#include <vector>

namespace {

// Wine exports these from ntdll with the cdecl calling convention.
using wine_get_version_t      = const char*(__cdecl*)(void);
using wine_get_build_id_t     = const char*(__cdecl*)(void);
using wine_get_host_version_t = void(__cdecl*)(const char**, const char**);

// Reads an environment variable, returning std::nullopt-style empty on absence.
std::wstring env(const wchar_t* name) {
    DWORD n = ::GetEnvironmentVariableW(name, nullptr, 0);
    if (n == 0) return {};
    std::wstring buf(n, L'\0');
    DWORD got = ::GetEnvironmentVariableW(name, buf.data(), n);
    buf.resize(got);
    return buf;
}

} // namespace

WineInfo detect_wine(Logger& log) {
    WineInfo info;

    HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    if (ntdll) {
        auto get_version = reinterpret_cast<wine_get_version_t>(
            reinterpret_cast<void*>(::GetProcAddress(ntdll, "wine_get_version")));
        if (get_version) {
            info.is_wine = true;
            info.version = strconv::from_ansi(get_version());

            if (auto get_build = reinterpret_cast<wine_get_build_id_t>(
                    reinterpret_cast<void*>(::GetProcAddress(ntdll, "wine_get_build_id"))))
                info.build_id = strconv::from_ansi(get_build());

            if (auto get_host = reinterpret_cast<wine_get_host_version_t>(
                    reinterpret_cast<void*>(::GetProcAddress(ntdll, "wine_get_host_version")))) {
                const char* sysname = nullptr;
                const char* release = nullptr;
                get_host(&sysname, &release);
                info.host_system  = strconv::from_ansi(sysname);
                info.host_release = strconv::from_ansi(release);
            }
        }
    }

    // Infer the higher-level stack from environment variables set by Steam/Proton,
    // UMU, and Lutris. None of these are authoritative alone, so we collect hints.
    std::vector<std::wstring> hints;
    auto note = [&](const wchar_t* var, const wchar_t* label) {
        std::wstring v = env(var);
        if (!v.empty()) hints.push_back(std::format(L"{}={}", label, v));
    };
    note(L"STEAM_COMPAT_DATA_PATH", L"steam-compat");
    note(L"SteamGameId",           L"steam-appid");
    note(L"STEAM_COMPAT_CLIENT_INSTALL_PATH", L"steam-client");
    note(L"PROTONPATH",            L"proton");        // umu / protonup
    note(L"PROTON_VERSION",        L"proton-ver");
    note(L"GAMEID",                L"umu-gameid");    // umu-launcher
    note(L"UMU_ID",                L"umu-id");
    note(L"WINEPREFIX",            L"wineprefix");
    note(L"WINE_GST_REGISTRY_DIR", L"wine-gst");
    note(L"LUTRIS_GAME_UUID",      L"lutris");

    if (!env(L"STEAM_COMPAT_DATA_PATH").empty() || !env(L"PROTONPATH").empty() ||
        !env(L"PROTON_VERSION").empty()) {
        info.is_proton = true;
    }
    if (!hints.empty()) {
        std::wstring joined;
        for (size_t i = 0; i < hints.size(); ++i) {
            if (i) joined += L"  ";
            joined += hints[i];
        }
        info.proton_hint = joined;
    }

    // Log the outcome.
    log.section(L"Environment detection");
    if (info.is_wine) {
        log.infof(L"Running under Wine {}", info.version.empty() ? L"<unknown>" : info.version);
        if (!info.build_id.empty())
            log.infof(L"  wine build id : {}", info.build_id);
        if (!info.host_system.empty())
            log.infof(L"  host system   : {} {}", info.host_system, info.host_release);
    } else {
        log.info(L"Not running under Wine (native Windows or Wine hid its markers)");
    }
    if (info.is_proton) log.info(L"  compatibility : Proton-style layer detected");
    if (!info.proton_hint.empty())
        log.infof(L"  env hints     : {}", info.proton_hint);

    return info;
}
