// Creates the gta_sa.exe process in a suspended state and later resumes it.
//
// Two decisions that differ from the original launcher and matter under
// Wine/Proton:
//   * STARTUPINFOW.cb is initialized (Wine is stricter than Windows here).
//   * The child's working directory is set explicitly to the game directory.
//     GTA:SA exits immediately if its cwd is not the game folder, and under
//     Steam/Proton/UMU the inherited cwd is usually NOT the game folder.
#pragma once

#include "win32_helpers.hpp"
#include <filesystem>
#include <optional>
#include <string>

class Logger;
class ErrorFormatter;

struct LaunchedProcess {
    win32::ScopedHandle process;
    win32::ScopedHandle thread;   // the process's main thread (still suspended)
    DWORD               pid = 0;
    DWORD               tid = 0;
};

class ProcessLauncher {
public:
    ProcessLauncher(Logger& log, ErrorFormatter& err) : log_(log), err_(err) {}

    // Launches `exe_path` with `command_line` (a full, mutable command line whose
    // argv[0] should be the target exe), working directory `work_dir`, suspended.
    std::optional<LaunchedProcess> create_suspended(const std::filesystem::path& exe_path,
                                                    const std::wstring& command_line,
                                                    const std::filesystem::path& work_dir);

    // Resumes the main thread. Returns false (and reports) on failure.
    bool resume(LaunchedProcess& p);

private:
    Logger&         log_;
    ErrorFormatter& err_;
};
