#include "windows_prelude.hpp"
#include "host_resolver.hpp"
#include "logger.hpp"
#include "strconv.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>

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

    WinsockScope ws;
    if (!ws.ok()) {
        log.warn(L"WSAStartup failed; forwarding the hostname unresolved");
        return r;
    }

    addrinfoW hints{};
    hints.ai_family = AF_INET;          // SA:MP is IPv4-only
    hints.ai_socktype = SOCK_DGRAM;
    addrinfoW* res = nullptr;
    if (::GetAddrInfoW(h.value.c_str(), nullptr, &hints, &res) != 0 || !res) {
        log.warnf(L"could not resolve {}; forwarding it unresolved", h.value);
        if (res) ::FreeAddrInfoW(res);
        return r;
    }

    std::vector<std::string> ips;
    for (addrinfoW* p = res; p && ips.size() < opts.max_candidates; p = p->ai_next) {
        if (p->ai_family != AF_INET || !p->ai_addr) continue;
        auto* sin = reinterpret_cast<sockaddr_in*>(p->ai_addr);
        char buf[INET_ADDRSTRLEN] = {};
        if (::InetNtopA(AF_INET, &sin->sin_addr, buf, sizeof(buf)))
            ips.emplace_back(buf);
    }
    ::FreeAddrInfoW(res);

    if (ips.empty()) {
        log.warn(L"resolver returned no IPv4 addresses; forwarding the hostname unresolved");
        return r;
    }
    log.infof(L"resolved       : {} address(es)", ips.size());

    // Probe in resolver order and take the FIRST that answers. Order matters:
    // it keeps the common case (healthy pool, first address fine) to a single
    // round-trip, and only walks the pool when the head of it is dead - which
    // is precisely the case this exists for.
    const auto start = std::chrono::steady_clock::now();
    for (const auto& ip : ips) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - start).count();
        if (elapsed >= static_cast<long long>(opts.total_budget_ms)) {
            log.warnf(L"probe budget ({} ms) exhausted after {} candidate(s)",
                      opts.total_budget_ms, r.candidates.size());
            break;
        }

        Candidate c;
        c.ip = strconv::to_wide(ip);
        if (auto rtt = probe(ip, port, opts.per_probe_ms)) {
            c.answered = true;
            c.rtt_ms = *rtt;
            r.candidates.push_back(c);
            r.chosen = c.ip;
            log.infof(L"  {} - answered in {} ms  <- using this", c.ip, c.rtt_ms);
            break;
        }
        r.candidates.push_back(c);
        log.infof(L"  {} - no reply in {} ms", c.ip, opts.per_probe_ms);
    }

    if (r.chosen.empty()) {
        // Every candidate silent. That is a server-is-down or a blocked-UDP
        // situation, not a resolver one, so hand the hostname through and let
        // the game report the failure it would have reported anyway.
        log.warnf(L"no address for {} answered a SA:MP query; forwarding the hostname unresolved",
                  h.value);
    } else if (r.candidates.size() > 1) {
        log.warnf(L"{} address(es) ahead of {} did not answer - the DNS pool has dead members",
                  r.candidates.size() - 1, r.chosen);
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
