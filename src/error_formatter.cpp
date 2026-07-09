#include "error_formatter.hpp"
#include "win32_helpers.hpp"
#include "logger.hpp"

#include <algorithm>
#include <format>

void ErrorFormatter::report(std::wstring_view title,
                            std::wstring_view summary,
                            const std::vector<Field>& fields) {
    // Log first: the log must contain the failure even if the MessageBox is
    // dismissed or never appears (headless run).
    log_.errorf(L"{}: {}", title, summary);

    // Compute a column width so the key/value block lines up.
    size_t key_w = 0;
    for (const auto& [k, v] : fields) key_w = std::max(key_w, k.size());

    std::wstring body{summary};
    body += L"\n";
    for (const auto& [k, v] : fields) {
        log_.errorf(L"    {:<16}: {}", k, v);
        body += std::format(L"\n{:<{}} : {}", k, key_w, v);
    }

    if (gui_) {
        ::MessageBoxW(nullptr, body.c_str(), std::wstring{title}.c_str(),
                      MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
    }
}

void ErrorFormatter::report_win32(std::wstring_view api,
                                  DWORD error,
                                  const std::vector<Field>& fields) {
    std::vector<Field> all;
    all.reserve(fields.size() + 1);
    all.emplace_back(L"Error", win32::format_error(error));
    for (const auto& f : fields) all.push_back(f);

    report(std::format(L"{} failed", api),
           std::format(L"{} failed with error {}", api, error),
           all);
}
