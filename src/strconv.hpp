// UTF-16 <-> UTF-8 conversion helpers.
//
// The launcher works in wide strings internally (Win32 is UTF-16, and SA:MP
// installs frequently live under non-ASCII paths). Logs are written as UTF-8 so
// they are readable in any editor and greppable on Linux.
#pragma once

#include <string>
#include <string_view>

namespace strconv {

std::string  to_utf8(std::wstring_view w);
std::wstring to_wide(std::string_view s);

// Narrow a C string that may be either UTF-8 or the ANSI code page (used for the
// char* returned by Wine's wine_get_* helpers, which are plain ASCII).
std::wstring from_ansi(const char* s);

} // namespace strconv
