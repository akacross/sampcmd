// Classic CreateRemoteThread + LoadLibraryW DLL injection, but with every stage
// validated and logged. The whole point of the rewrite is that injection can no
// longer fail silently:
//
//   * VirtualAllocEx result checked (and RAII-freed).
//   * WriteProcessMemory byte count checked.
//   * The remote thread's exit code is read back — for a LoadLibraryW thread that
//     value IS the HMODULE returned inside the target. Zero means LoadLibrary
//     failed inside gta_sa.exe, which we report instead of resuming blindly.
#pragma once

#include "windows_prelude.hpp"
#include <filesystem>
#include <string>

class Logger;
class ErrorFormatter;

struct InjectionResult {
    bool         success = false;
    std::wstring failed_stage;         // e.g. "WriteProcessMemory"
    DWORD        last_error = 0;        // GetLastError at the failing stage
    DWORD        thread_exit = 0;       // GetExitCodeThread => remote HMODULE (low 32 bits)
    void*        remote_module = nullptr;
};

class Injector {
public:
    Injector(Logger& log, ErrorFormatter& err) : log_(log), err_(err) {}

    // Injects `dll_path` (absolute, already validated to exist) into `process`.
    InjectionResult inject(HANDLE process, DWORD pid, const std::filesystem::path& dll_path);

private:
    Logger&         log_;
    ErrorFormatter& err_;
};
