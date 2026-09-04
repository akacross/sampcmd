#include "windows_prelude.hpp"
#include "host_resolver.hpp"
#include "host_cache.hpp"
#include "logger.hpp"
#include "strconv.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <chrono>
#include <cstring>

namespace hostres {
namespace {

// SA:MP query packet: "SAMP" + 4 address bytes + port (little-endian) + opcode.
// 'i' is the info query - the same one a server browser sends - and every live
// SA:MP or open.mp server answers it. Nothing else on the port does, which is
// what makes it a proof of "this is the server" rather than merely "something
// is listening".
constexpr char kOpcodeInfo = 'i';

bool is_ip_literal(const std::wstring& host) {
    in_addr a{};
    return ::InetPtonW(AF_INET, host.c_str(), &a) == 1;
}

// RAII for WSAStartup - the launcher does no other networking, so winsock is
// brought up only for this and torn down immediately after.
class WinsockScope {
public:
    WinsockScope() {
        WSADATA d{};
        ok_ = ::WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }
    ~WinsockScope() { if (ok_) ::WSACleanup(); }
    bool ok() const { return ok_; }
    WinsockScope(const WinsockScope&) = delete;
    WinsockScope& operator=(const WinsockScope&) = delete;
private:
    bool ok_ = false;
};

unsigned parse_port(const std::vector<std::wstring>& args) {
    for (size_t i = 0; i < args.size(); ++i) {
        std::wstring v;
        if (args[i] == L"-p" && i + 1 < args.size())      v = args[i + 1];
        else if (args[i].rfind(L"-p", 0) == 0 && args[i].size() > 2) v = args[i].substr(2);
        else continue;
        try {
            unsigned long p = std::stoul(v);
            if (p > 0 && p < 65536) return static_cast<unsigned>(p);
        } catch (...) { /* fall through to the default */ }
    }
    return 7777;  // SA:MP's default, and what the server list assumes
}

// Locates the -h value. Returns npos-ish (index == args.size()) when absent.
struct HostArg { size_t index = 0; bool separate = false; std::wstring value; bool found = false; };

HostArg find_host(const std::vector<std::wstring>& args) {
    HostArg h;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == L"-h" && i + 1 < args.size()) {
            h.index = i + 1; h.separate = true; h.value = args[i + 1]; h.found = true;
            return h;  // last-wins would be surprising; SA:MP reads the first
        }
        if (args[i].rfind(L"-h", 0) == 0 && args[i].size() > 2) {
            h.index = i; h.separate = false; h.value = args[i].substr(2); h.found = true;
            return h;
        }
    }
    return h;
}

// One UDP query/response. Returns round-trip ms, or nothing on timeout.
std::optional<unsigned> probe(const std::string& ip, unsigned port, unsigned timeout_ms) {
    in_addr addr{};
    if (::InetPtonA(AF_INET, ip.c_str(), &addr) != 1) return std::nullopt;

    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return std::nullopt;

    DWORD tv = timeout_ms;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));

    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = ::htons(static_cast<unsigned short>(port));
    sa.sin_addr = addr;

    // Payload is little-endian on the wire regardless of host order, which on
    // x86 is the same thing; spelled out so it stays correct if that changes.
    char pkt[11];
    std::memcpy(pkt, "SAMP", 4);
    const unsigned long a = ::ntohl(addr.s_addr);
    pkt[4] = static_cast<char>((a >> 24) & 0xFF);
    pkt[5] = static_cast<char>((a >> 16) & 0xFF);
    pkt[6] = static_cast<char>((a >> 8) & 0xFF);
    pkt[7] = static_cast<char>(a & 0xFF);
    pkt[8] = static_cast<char>(port & 0xFF);
    pkt[9] = static_cast<char>((port >> 8) & 0xFF);
    pkt[10] = kOpcodeInfo;

    const auto t0 = std::chrono::steady_clock::now();
    int sent = ::sendto(s, pkt, sizeof(pkt), 0,
                        reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
    if (sent != static_cast<int>(sizeof(pkt))) { ::closesocket(s); return std::nullopt; }

    char buf[512];
    int got = ::recv(s, buf, sizeof(buf), 0);
    ::closesocket(s);

    // A valid reply echoes the 11-byte header. Anything shorter, or an ICMP
    // port-unreachable surfacing as an error, means no server here.
    if (got < 11 || std::memcmp(buf, "SAMP", 4) != 0) return std::nullopt;

    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();
    return static_cast<unsigned>(ms);
}

// Human-friendly age for the log: "42 s", "9 min", "3 h", "2 d".
std::wstring age_text(long long seconds) {
    if (seconds < 0) seconds = 0;
    if (seconds < 120)   return std::to_wstring(seconds) + L" s";
    if (seconds < 7200)  return std::to_wstring(seconds / 60) + L" min";
    if (seconds < 172800)return std::to_wstring(seconds / 3600) + L" h";
    return std::to_wstring(seconds / 86400) + L" d";
}

} // namespace

Result resolve_host_arg(const std::vector<std::wstring>& args, Logger& log,
                        const Options& opts) {
    Result r;

    HostArg h = find_host(args);
    if (!h.found || h.value.empty()) return r;      // nothing to resolve
    r.original = h.value;

    if (is_ip_literal(h.value)) {
        log.debugf(L"host resolve   : {} is already an IP, forwarding as-is", h.value);
        return r;                                    // attempted stays false
    }

    r.attempted = true;
    const unsigned port = parse_port(args);
    log.section(L"Host resolve");
    log.infof(L"host           : {} (port {})", h.value, port);

    // What we learned about this host last time. Everything below can run
    // without it; it only ever shortens the work or rescues a failure.
    const auto cached = hostcache::load(opts.cache_path, h.value, port);
    const long long now = hostcache::now_unix();
    const long long cached_age = cached ? now - cached->proven_at : 0;

    if (cached && !cached->ip.empty() && cached->pinned) {
        // A hand-written line. The user has said "this host is that address";
        // second-guessing it with DNS would defeat the point of writing it.
        r.chosen = cached->ip;
        r.source = Source::Pinned;
        log.infof(L"pinned         : {} (cache entry says pin, no lookup performed)", r.chosen);
        return r;
    }

    if (cached && !cached->ip.empty() && cached_age >= 0 &&
        cached_age <= static_cast<long long>(opts.cache_trust_s)) {
        // Proven minutes ago. Relaunching the server you were just on is the
        // common case after a crash, and nothing about it is worth a round trip.
        r.chosen = cached->ip;
        r.source = Source::Fresh;
        log.infof(L"cache hit      : {} - answered {} ago in {} ms; skipping DNS and probe",
                  r.chosen, age_text(cached_age), cached->rtt_ms);
        return r;
    }

    // Falls back to the last proven address when this launch cannot prove one.
    // Only used when DNS still lists it (or DNS said nothing at all): if the
    // pool has genuinely moved on, an address from yesterday is not a rescue,
    // it is a wrong answer.
    auto use_fallback = [&](const std::vector<std::string>& resolved, const wchar_t* why) {
        if (!cached || cached->ip.empty()) return false;
        if (cached_age < 0 || cached_age > static_cast<long long>(opts.cache_ttl_s)) return false;
        if (!resolved.empty()) {
            const std::string want = strconv::to_utf8(cached->ip);
            if (std::find(resolved.begin(), resolved.end(), want) == resolved.end()) {
                log.warnf(L"cached address {} is no longer in DNS for {}; forgetting it",
                          cached->ip, h.value);
                hostcache::erase(opts.cache_path, h.value, port);
                return false;
            }
        }
        if (cached->fails + 1 > opts.max_fallbacks) {
            log.warnf(L"cached address {} has now gone {} launches without answering; forgetting it",
                      cached->ip, cached->fails + 1);
            hostcache::erase(opts.cache_path, h.value, port);
            return false;
        }
        hostcache::Entry e = *cached;
        e.fails += 1;
        hostcache::store(opts.cache_path, e);

        r.chosen = cached->ip;
        r.source = Source::Fallback;
        log.warnf(L"{}; using the last address that did: {} (proven {} ago, unproven {}x)",
                  why, r.chosen, age_text(cached_age), e.fails);
        return true;
    };

    WinsockScope ws;
    if (!ws.ok()) {
        log.warn(L"WSAStartup failed; no probe possible");
        use_fallback({}, L"winsock is unavailable");
        return r;
    }

    addrinfoW hints{};
    hints.ai_family = AF_INET;          // SA:MP is IPv4-only
    hints.ai_socktype = SOCK_DGRAM;
    addrinfoW* res = nullptr;
    std::vector<std::string> ips;

    // The whole answer, not just the slice we will probe: a round-robin pool can
    // be larger than max_candidates, and "is the cached address still in DNS?"
    // must not be decided from a truncated list - that would forget a perfectly
    // good address whenever the rotation pushed it past the cut.
    if (::GetAddrInfoW(h.value.c_str(), nullptr, &hints, &res) == 0 && res) {
        for (addrinfoW* p = res; p; p = p->ai_next) {
            if (p->ai_family != AF_INET || !p->ai_addr) continue;
            auto* sin = reinterpret_cast<sockaddr_in*>(p->ai_addr);
            char buf[INET_ADDRSTRLEN] = {};
            if (::InetNtopA(AF_INET, &sin->sin_addr, buf, sizeof(buf)))
                ips.emplace_back(buf);
        }
    }
    if (res) ::FreeAddrInfoW(res);

    // Resolver order, as returned. Kept for the "is the cached address still
    // listed?" test above, which must not see our own reordering.
    const std::vector<std::string> resolved = ips;

    if (ips.empty()) {
        // DNS is down, or the name has stopped resolving. The addresses from the
        // last successful lookup are still the best guess available, and every
        // one of them is about to be proven anyway.
        if (cached && !cached->addresses.empty()) {
            for (const auto& a : cached->addresses) ips.push_back(strconv::to_utf8(a));
            log.warnf(L"could not resolve {}; probing the {} address(es) cached from the last lookup",
                      h.value, ips.size());
        } else {
            log.warnf(L"could not resolve {}; forwarding it unresolved", h.value);
            use_fallback({}, L"the name did not resolve");
            return r;
        }
    } else {
        log.infof(L"resolved       : {} address(es)", ips.size());
    }

    // The address that answered last time goes to the head of the queue: it is
    // the one most likely to answer again, and putting it first means the usual
    // launch is a single round trip even when the pool's DNS order is bad. This
    // happens BEFORE the max_candidates cut, so a big pool cannot rotate the
    // known-good address out of the run.
    if (cached && !cached->ip.empty()) {
        const std::string want = strconv::to_utf8(cached->ip);
        auto it = std::find(ips.begin(), ips.end(), want);
        if (it != ips.end() && it != ips.begin()) {
            std::rotate(ips.begin(), it, it + 1);
            log.debugf(L"trying {} first - it answered {} ago", cached->ip, age_text(cached_age));
        }
    }
    if (ips.size() > opts.max_candidates) {
        log.debugf(L"probing the first {} of {} address(es)", opts.max_candidates, ips.size());
        ips.resize(opts.max_candidates);
    }

    // Probe in that order, `attempts` times over, and take the FIRST address that
    // answers. Walking the list repeatedly is what makes a dropped datagram cost
    // another round trip instead of costing the launch: measured, one real server
    // silently discards 10-15% of queries while answering in ~118 ms.
    std::vector<Candidate> state;
    state.reserve(ips.size());
    for (const auto& ip : ips) {
        Candidate c;
        c.ip = strconv::to_wide(ip);
        state.push_back(c);
    }

    const auto start = std::chrono::steady_clock::now();
    bool out_of_budget = false;
    for (unsigned round = 0; round < opts.attempts && r.chosen.empty() && !out_of_budget; ++round) {
        for (size_t i = 0; i < ips.size(); ++i) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - start).count();
            if (elapsed >= static_cast<long long>(opts.total_budget_ms)) {
                log.warnf(L"probe budget ({} ms) exhausted on attempt {} of {}",
                          opts.total_budget_ms, round + 1, opts.attempts);
                out_of_budget = true;
                break;
            }

            state[i].attempts += 1;
            if (auto rtt = probe(ips[i], port, opts.per_probe_ms)) {
                state[i].answered = true;
                state[i].rtt_ms = *rtt;
                r.chosen = state[i].ip;
                r.source = Source::Probe;
                log.infof(L"  {} - answered in {} ms  <- using this", state[i].ip, *rtt);
                break;
            }
            log.infof(L"  {} - no reply in {} ms (attempt {} of {})",
                      state[i].ip, opts.per_probe_ms, round + 1, opts.attempts);
        }
    }
    r.candidates = state;

    if (!r.chosen.empty()) {
        unsigned rtt = 0;
        unsigned dead_ahead = 0;
        for (const auto& c : state) {
            if (c.ip == r.chosen) { rtt = c.rtt_ms; break; }
            if (c.attempts) ++dead_ahead;
        }
        if (dead_ahead)
            log.warnf(L"{} address(es) ahead of {} did not answer - the DNS pool has dead members",
                      dead_ahead, r.chosen);

        hostcache::Entry e;
        e.host = h.value;
        e.port = port;
        e.ip = r.chosen;
        e.proven_at = now;
        e.rtt_ms = rtt;
        e.fails = 0;
        for (const auto& a : resolved) e.addresses.push_back(strconv::to_wide(a));
        if (e.addresses.empty() && cached) e.addresses = cached->addresses;
        hostcache::store(opts.cache_path, e);
        return r;
    }

    // Every candidate silent, every attempt. Either the server is genuinely down
    // or UDP is blocked here - but if this host answered recently, silence is
    // more likely to be the network than the truth, so the remembered address
    // wins over giving up.
    if (!use_fallback(resolved, L"no address answered a SA:MP query")) {
        log.warnf(L"no address for {} answered a SA:MP query; forwarding the hostname unresolved",
                  h.value);
    }
    return r;
}

void apply(std::vector<std::wstring>& args, const Result& r) {
    if (!r.attempted || r.chosen.empty()) return;

    HostArg h = find_host(args);
    if (!h.found) return;
    if (h.separate) args[h.index] = r.chosen;
    else            args[h.index] = L"-h" + r.chosen;
}

} // namespace hostres
