#include "logger.hpp"
#include "strconv.hpp"

#include <format>

namespace {

const wchar_t* level_name(Logger::Level l) {
    switch (l) {
        case Logger::Level::Trace: return L"TRACE";
        case Logger::Level::Debug: return L"DEBUG";
        case Logger::Level::Info:  return L"INFO";
        case Logger::Level::Warn:  return L"WARN";
        case Logger::Level::Error: return L"ERROR";
    }
    return L"?????";
}

std::wstring timestamp() {
    SYSTEMTIME t;
    ::GetLocalTime(&t);
    return std::format(L"{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03}",
                       t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
}

// Minimal JSON string escaping for the --json log mode.
std::wstring json_escape(std::wstring_view s) {
    std::wstring out;
    out.reserve(s.size() + 8);
    for (wchar_t c : s) {
        switch (c) {
            case L'"':  out += L"\\\""; break;
            case L'\\': out += L"\\\\"; break;
            case L'\n': out += L"\\n";  break;
            case L'\r': out += L"\\r";  break;
            case L'\t': out += L"\\t";  break;
            default:
                if (c < 0x20) out += std::format(L"\\u{:04x}", static_cast<unsigned>(c));
                else          out += c;
        }
    }
    return out;
}

} // namespace

Logger::~Logger() {
    if (file_ != INVALID_HANDLE_VALUE) ::CloseHandle(file_);
}

void Logger::open(const Options& opts) {
    console_   = opts.console;
    json_      = opts.json;
    min_level_ = opts.min_level;
    file_path_ = opts.file_path;

    if (!file_path_.empty()) {
        // Truncate on open: each launch starts a fresh, self-contained log.
        file_ = ::CreateFileW(file_path_.c_str(), GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ != INVALID_HANDLE_VALUE) {
            // UTF-8 BOM so editors detect the encoding of non-ASCII paths.
            const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
            DWORD written = 0;
            ::WriteFile(file_, bom, sizeof(bom), &written, nullptr);
        }
    }
}

void Logger::write_line(std::wstring_view line) {
    if (file_ != INVALID_HANDLE_VALUE) {
        std::string utf8 = strconv::to_utf8(line);
        utf8 += '\n';
        DWORD written = 0;
        ::WriteFile(file_, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    }
    if (console_) {
        std::wstring text{line};
        text += L'\n';
        DWORD written = 0;
        HANDLE out = ::GetStdHandle(STD_OUTPUT_HANDLE);
        if (out && out != INVALID_HANDLE_VALUE) {
            // WriteConsoleW for a real console; fall back to UTF-8 bytes when the
            // handle is a redirected file/pipe.
            DWORD mode = 0;
            if (::GetConsoleMode(out, &mode)) {
                ::WriteConsoleW(out, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
            } else {
                std::string utf8 = strconv::to_utf8(text);
                ::WriteFile(out, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
            }
        }
    }
}

void Logger::log(Level level, std::wstring_view msg) {
    if (static_cast<int>(level) < static_cast<int>(min_level_)) return;

    if (json_) {
        write_line(std::format(LR"({{"ts":"{}","level":"{}","msg":"{}"}})",
                               timestamp(), level_name(level), json_escape(msg)));
    } else {
        write_line(std::format(L"[{}] {:<5} {}", timestamp(), level_name(level), msg));
    }
}

void Logger::section(std::wstring_view title) {
    if (json_) { log(Level::Info, title); return; }
    write_line(L"");
    write_line(std::format(L"==== {} ====", title));
}

void Logger::raw(std::wstring_view block) {
    if (json_) { log(Level::Info, block); return; }
    write_line(block);
}
