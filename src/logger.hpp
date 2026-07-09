// Structured logger: writes a timestamped launcher.log and optionally mirrors to
// a console. Everything the launcher does flows through here so that a failure in
// the field can be diagnosed from the log alone.
#pragma once

#include "windows_prelude.hpp"
#include <format>
#include <string>
#include <string_view>
#include <utility>

class Logger {
public:
    enum class Level { Trace, Debug, Info, Warn, Error };

    struct Options {
        std::wstring file_path;            // "" => no file
        bool         console   = false;    // mirror to stdout/stderr
        bool         json      = false;    // emit JSON-lines instead of text
        Level        min_level = Level::Info;
    };

    Logger() = default;
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    // Opens the log file (append/truncate per session). Safe to call once.
    void open(const Options& opts);

    void log(Level level, std::wstring_view msg);

    void trace(std::wstring_view m) { log(Level::Trace, m); }
    void debug(std::wstring_view m) { log(Level::Debug, m); }
    void info(std::wstring_view m)  { log(Level::Info,  m); }
    void warn(std::wstring_view m)  { log(Level::Warn,  m); }
    void error(std::wstring_view m) { log(Level::Error, m); }

    // std::format-based variants keep call sites tidy.
    template <class... A> void tracef(std::wformat_string<A...> f, A&&... a) { log(Level::Trace, std::format(f, std::forward<A>(a)...)); }
    template <class... A> void debugf(std::wformat_string<A...> f, A&&... a) { log(Level::Debug, std::format(f, std::forward<A>(a)...)); }
    template <class... A> void infof(std::wformat_string<A...> f, A&&... a)  { log(Level::Info,  std::format(f, std::forward<A>(a)...)); }
    template <class... A> void warnf(std::wformat_string<A...> f, A&&... a)  { log(Level::Warn,  std::format(f, std::forward<A>(a)...)); }
    template <class... A> void errorf(std::wformat_string<A...> f, A&&... a) { log(Level::Error, std::format(f, std::forward<A>(a)...)); }

    // Visual separators for the human-readable log.
    void section(std::wstring_view title);

    // Writes a block verbatim (no timestamp/level prefix). Used for the paste-ready
    // report so it can be copied cleanly. Multi-line blocks are preserved.
    void raw(std::wstring_view block);

    [[nodiscard]] const std::wstring& file_path() const noexcept { return file_path_; }

private:
    void write_line(std::wstring_view utf16_line);

    HANDLE       file_   = INVALID_HANDLE_VALUE;
    bool         console_ = false;
    bool         json_    = false;
    Level        min_level_ = Level::Info;
    std::wstring file_path_;
};
