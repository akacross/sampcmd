// Turns a failure into a structured, human-readable report and surfaces it via
// the log plus (in GUI mode) a MessageBox laid out like:
//
//     <summary>
//
//     Error:
//     (0x....) The system cannot find the file specified.
//
//     GTA:            <path>
//     Current Dir:    <path>
//     ...
#pragma once

#include "windows_prelude.hpp"
#include <string>
#include <utility>
#include <vector>

class Logger;

class ErrorFormatter {
public:
    // gui_allowed=false suppresses MessageBox (console/headless mode).
    ErrorFormatter(Logger& log, bool gui_allowed) : log_(log), gui_(gui_allowed) {}

    using Field = std::pair<std::wstring, std::wstring>;

    // Reports a failure. `summary` is the one-line what-failed; `fields` are the
    // key/value context lines (paths, PIDs, error strings, ...).
    void report(std::wstring_view title,
                std::wstring_view summary,
                const std::vector<Field>& fields);

    // Convenience for "<api> failed" with the given Win32 error prefilled.
    void report_win32(std::wstring_view api,
                      DWORD error,
                      const std::vector<Field>& fields);

private:
    Logger& log_;
    bool    gui_;
};
