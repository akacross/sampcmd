// Collects and logs a snapshot of the runtime environment: OS version, process
// architecture, module/working paths, and the raw command line. Written once at
// startup so every log answers "what machine, what build, what invocation".
#pragma once

#include <filesystem>
#include <string>

class Logger;

namespace diagnostics {

// Absolute path of the running launcher executable (GetModuleFileNameW(NULL)).
std::filesystem::path self_executable_path();

// Directory that contains the launcher executable.
std::filesystem::path self_directory();

// Real OS version via RtlGetVersion (GetVersionEx is capped without a manifest).
std::wstring os_version_string();

// "x86 (32-bit)" / "x64" plus WOW64 status for this process.
std::wstring process_arch_string();

// Dumps everything to the log under a "Diagnostics" section.
void log_snapshot(Logger& log);

} // namespace diagnostics
