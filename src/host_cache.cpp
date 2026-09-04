#include "windows_prelude.hpp"
#include "host_cache.hpp"
#include "strconv.hpp"

#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

namespace hostcache {
namespace {

constexpr wchar_t kHeader[] =
    L"# sampcmd host cache - addresses proven to answer a SA:MP query.\n"
    L"#\n"
    L"# host <tab> port <tab> address <tab> proven-at <tab> rtt_ms <tab> fails <tab> pool\n"
    L"#\n"
    L"# proven-at is unix seconds. Write \"pin\" there instead and the address is\n"
    L"# used verbatim on every launch - no DNS, no probe, no expiry:\n"
    L"#\n"
    L"#   myserver.net\t7777\t203.0.113.10\tpin\n"
    L"#\n"
    L"# Delete a line to forget it; the launcher rewrites this file as it learns.\n";

std::wstring lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

std::vector<std::wstring> split(const std::wstring& s, wchar_t sep) {
    std::vector<std::wstring> out;
    size_t start = 0;
    for (size_t i = 0;; ++i) {
        if (i == s.size() || s[i] == sep) {
            out.push_back(s.substr(start, i - start));
            if (i == s.size()) break;
            start = i + 1;
        }
    }
    return out;
}

std::wstring trim(const std::wstring& s) {
    size_t b = s.find_first_not_of(L" \t\r\n");
    if (b == std::wstring::npos) return {};
    size_t e = s.find_last_not_of(L" \t\r\n");
    return s.substr(b, e - b + 1);
}

// Whole-file read. The cache is a handful of lines, so there is no reason to
// stream it, and reading it in one shot keeps the parse trivially restartable
// if a line turns out to be malformed.
std::wstring read_file(const std::wstring& path) {
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::string bytes;
    char buf[4096];
    DWORD got = 0;
    while (::ReadFile(h, buf, sizeof(buf), &got, nullptr) && got > 0)
        bytes.append(buf, got);
    ::CloseHandle(h);
    if (bytes.rfind("\xEF\xBB\xBF", 0) == 0) bytes.erase(0, 3);  // tolerate a BOM
    return strconv::to_wide(bytes);
}

// Written via a temporary and a replacing rename so a launch that dies mid-write
// (or two launchers racing) can never leave a half-file behind.
bool write_file(const std::wstring& path, const std::wstring& text) {
    const std::wstring tmp = path + L".tmp";
    HANDLE h = ::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const std::string utf8 = strconv::to_utf8(text);
    DWORD written = 0;
    const BOOL ok = ::WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    ::CloseHandle(h);
    if (!ok || written != utf8.size()) { ::DeleteFileW(tmp.c_str()); return false; }
    if (!::MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        ::DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

std::optional<Entry> parse_line(const std::wstring& line) {
    const std::wstring t = trim(line);
    if (t.empty() || t[0] == L'#') return std::nullopt;

    // Tab-separated, but a hand-edited line may well use spaces, so accept any
    // run of whitespace as a separator.
    std::vector<std::wstring> f;
    size_t i = 0;
    while (i < t.size()) {
        while (i < t.size() && (t[i] == L' ' || t[i] == L'\t')) ++i;
        size_t b = i;
        while (i < t.size() && t[i] != L' ' && t[i] != L'\t') ++i;
        if (i > b) f.push_back(t.substr(b, i - b));
    }
    if (f.size() < 3) return std::nullopt;

    Entry e;
    e.host = lower(f[0]);
    try {
        unsigned long p = std::stoul(f[1]);
        if (p == 0 || p > 65535) return std::nullopt;
        e.port = static_cast<unsigned>(p);
    } catch (...) { return std::nullopt; }
    e.ip = f[2];

    if (f.size() > 3) {
        if (lower(f[3]) == L"pin") e.pinned = true;
        else try { e.proven_at = std::stoll(f[3]); } catch (...) { return std::nullopt; }
    } else {
        e.pinned = true;  // "host port ip" alone reads as a hosts-file entry
    }
    if (f.size() > 4) try { e.rtt_ms = static_cast<unsigned>(std::stoul(f[4])); } catch (...) {}
    if (f.size() > 5) try { e.fails = static_cast<unsigned>(std::stoul(f[5])); } catch (...) {}
    if (f.size() > 6)
        for (const auto& a : split(f[6], L','))
            if (!a.empty()) e.addresses.push_back(a);
    return e;
}

std::wstring format_line(const Entry& e) {
    std::wstring pool;
    for (size_t i = 0; i < e.addresses.size(); ++i) {
        if (i) pool.push_back(L',');
        pool += e.addresses[i];
    }
    std::wstring stamp = e.pinned ? L"pin" : std::to_wstring(e.proven_at);
    return e.host + L"\t" + std::to_wstring(e.port) + L"\t" + e.ip + L"\t" + stamp +
           L"\t" + std::to_wstring(e.rtt_ms) + L"\t" + std::to_wstring(e.fails) +
           L"\t" + (pool.empty() ? L"-" : pool) + L"\n";
}

// Rewrites the file with `e` replacing any line for the same (host, port).
// `e == nullptr` deletes instead. Comment lines are carried through so a user's
// own notes and pins survive us rewriting around them.
void upsert(const std::wstring& path, const std::wstring& host, unsigned port,
            const Entry* e) {
    if (path.empty()) return;

    const std::wstring key = lower(host);
    std::wstring out;
    bool replaced = false;

    const std::wstring existing = read_file(path);
    if (existing.empty()) {
        out = kHeader;
    } else {
        for (const auto& raw : split(existing, L'\n')) {
            const std::wstring line = raw.empty() || raw.back() != L'\r'
                                          ? raw : raw.substr(0, raw.size() - 1);
            const std::wstring t = trim(line);
            if (t.empty()) continue;
            if (t[0] == L'#') { out += line; out.push_back(L'\n'); continue; }

            auto parsed = parse_line(line);
            if (parsed && parsed->host == key && parsed->port == port) {
                if (e) { out += format_line(*e); replaced = true; }
                continue;  // dropped when e == nullptr
            }
            out += line;
            out.push_back(L'\n');
        }
        if (out.empty()) out = kHeader;
    }

    if (e && !replaced) out += format_line(*e);
    write_file(path, out);
}

} // namespace

long long now_unix() {
    FILETIME ft{};
    ::GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u{};
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    // FILETIME counts 100 ns ticks from 1601-01-01; 11644473600 s to the epoch.
    return static_cast<long long>(u.QuadPart / 10000000ULL) - 11644473600LL;
}

std::optional<Entry> load(const std::wstring& path, const std::wstring& host,
                          unsigned port) {
    if (path.empty()) return std::nullopt;
    const std::wstring key = lower(host);
    const std::wstring text = read_file(path);
    if (text.empty()) return std::nullopt;

    for (const auto& line : split(text, L'\n')) {
        auto e = parse_line(line);
        if (e && e->host == key && e->port == port) return e;
    }
    return std::nullopt;
}

void store(const std::wstring& path, const Entry& e) {
    Entry norm = e;
    norm.host = lower(e.host);
    upsert(path, norm.host, norm.port, &norm);
}

void erase(const std::wstring& path, const std::wstring& host, unsigned port) {
    upsert(path, host, port, nullptr);
}

} // namespace hostcache
