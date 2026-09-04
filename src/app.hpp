// The Launcher: orchestrates CLI -> logging -> diagnostics -> validation ->
// launch -> injection -> resume. Thin by design; each responsibility lives in
// its own module.
#pragma once

#include "cli.hpp"
#include "logger.hpp"
#include <filesystem>

class App {
public:
    // Runs the whole flow. Returns a process exit code (0 == success).
    int run();

private:
    // Resolves game_dir / exe / dll / log paths from options + launcher location.
    struct Paths {
        std::filesystem::path game_dir;
        std::filesystem::path exe;
        std::filesystem::path dll;
        std::filesystem::path omp;   // omp-client.dll (only used with -omp)
        std::filesystem::path log;
        std::filesystem::path host_cache;  // proven -h addresses (sampcmd.hosts)
    };
    Paths resolve_paths(const CliOptions& opts) const;

    // Builds the command line handed to gta_sa.exe: argv[0] = target exe,
    // followed by the forwarded SA:MP arguments (properly quoted).
    static std::wstring build_child_command_line(const std::filesystem::path& exe,
                                                 const std::vector<std::wstring>& forwarded);

    // GetFileAttributesW existence check with a friendly log line.
    bool require_file(const std::filesystem::path& p, std::wstring_view label);

    Logger logger_;
};
