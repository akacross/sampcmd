// On-disk memory of which address a SA:MP hostname actually answers on.
//
// WHY THIS EXISTS
// ---------------
// host_resolver proves an address before handing it to GTA by sending the
// server the same query a server browser sends. That proof is worth keeping:
//
//   * A SA:MP query is a single UDP datagram with no retransmission, and real
//     servers drop them. Measured 2026-08-16 against samp.hzgaming.net:
//     213.32.6.232 answers in ~118 ms but silently discards 10-15% of queries,
//     with or without pacing - so a one-shot probe declares a perfectly healthy
//     server dead roughly one launch in seven. Remembering the last proven
//     address turns that into a fallback instead of a failure.
//   * The proof costs a round trip. Relaunching the same server (after a crash,
//     or just playing on one server all evening) should not pay it again.
//   * A hostname whose DNS is briefly unreachable still has a known-good
//     address from the last launch, so the launcher can carry on regardless.
//
// The file is deliberately plain text: it doubles as a hosts file for this
// launcher. A line whose timestamp field reads "pin" is used verbatim, forever,
// with no DNS and no probe - the way to hard-wire a server whose DNS you do not
// trust.
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace hostcache {

struct Entry {
    std::wstring host;                    // lower-cased -h value
    unsigned port = 0;                    // the -p value it was proven on
    std::wstring ip;                      // the address that answered
    long long proven_at = 0;              // unix seconds; 0 with pinned=true
    unsigned rtt_ms = 0;                  // what it answered in, for the log
    unsigned fails = 0;                   // consecutive uses without fresh proof
    bool pinned = false;                  // hand-written line: trust unconditionally
    std::vector<std::wstring> addresses;  // last resolver answer, for DNS outages
};

// Seconds since the unix epoch, from the system clock.
long long now_unix();

// Reads the entry for (host, port). Returns nothing if the file is missing,
// unreadable, or has no matching line. Never throws.
std::optional<Entry> load(const std::wstring& path, const std::wstring& host,
                          unsigned port);

// Inserts or replaces the entry for (e.host, e.port) and rewrites the file.
// Pinned lines and comments the user wrote are preserved. Failure is silent -
// a cache that cannot be written must not stop a launch.
void store(const std::wstring& path, const Entry& e);

// Drops the entry for (host, port), used when it has stopped being true.
void erase(const std::wstring& path, const std::wstring& host, unsigned port);

} // namespace hostcache
