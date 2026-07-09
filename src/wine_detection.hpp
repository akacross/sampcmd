// Runtime detection of Wine / Proton / compatibility-layer environments.
//
// Per the current plan this only *observes and logs* — it does not (yet) change
// launcher behavior. Having it recorded in every log makes the Windows-vs-Wine
// variable explicit when triaging a failure.
#pragma once

#include <string>

class Logger;

struct WineInfo {
    bool         is_wine = false;
    std::wstring version;       // wine_get_version()      e.g. "11.12"
    std::wstring build_id;      // wine_get_build_id()
    std::wstring host_system;   // wine_get_host_version() sysname, e.g. "Linux"
    std::wstring host_release;  // wine_get_host_version() release

    // Higher-level compatibility stack, inferred from environment variables.
    bool         is_proton = false;
    std::wstring proton_hint;   // human-readable summary of what was detected
};

// Detects Wine and logs everything it finds. Never throws.
WineInfo detect_wine(Logger& log);
