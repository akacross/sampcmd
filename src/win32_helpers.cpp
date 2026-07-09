#include "win32_helpers.hpp"

#include <format>

namespace win32 {

std::wstring format_error(DWORD error) {
    LPWSTR buffer = nullptr;
    const DWORD len = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);

    std::wstring message;
    if (len && buffer) {
        message.assign(buffer, len);
        // Trim trailing CR/LF/space FormatMessage likes to append.
        while (!message.empty() &&
               (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' '))
            message.pop_back();
    } else {
        message = L"<no system message>";
    }
    if (buffer) ::LocalFree(buffer);

    return std::format(L"(0x{:08X}) {}", error, message);
}

std::wstring last_error() { return format_error(::GetLastError()); }

} // namespace win32
