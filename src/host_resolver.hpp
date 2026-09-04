// Resolve the SA:MP "-h <host>" argument to an address the server actually
// answers on, before gta_sa.exe ever sees it.
//
// WHY THIS EXISTS
// ---------------
// SA:MP takes the FIRST address the resolver hands it and connects there. Big
// community servers publish a round-robin pool of A records, and those pools
// routinely contain a member that no longer serves - the DNS entry outlives the
// machine. Wine resolves through getaddrinfo, whose RFC 3484 ordering is stable
// for a given client, so if the dead member happens to sort first you do not get
// an intermittent failure: you get "Failed to connect to host" every single time,
// on a hostname that looks perfectly healthy in a browser.
//
// Observed case (2026-08-16): s2.gta-mp.cz publishes 10 A records. Nine answer a
// SA:MP query on :7777; 178.32.234.16 answers nothing, and getaddrinfo sorted it
// first on every lookup.
//
// So resolving alone is not enough - the stock samp.exe resolves too, and would
// pick the same dead address. The address has to be PROVEN before it is used,
// which is what the probe below does.
//
// WHY THE PROOF IS RETRIED AND REMEMBERED
// ---------------------------------------
// The first version probed each address exactly once and trusted the silence.
// That is not safe: a SA:MP query is one UDP datagram, and servers drop them.
// Measured 2026-08-16, 20 queries per run, paced and unpaced:
//
//     samp.hzgaming.net (213.32.6.232)   17/20 and 18/20 answered, all ~118 ms
//     s2.gta-mp.cz      (51.254.21.27)   20/20 answered, all ~118 ms
//
// So hzgaming - a live server with a single A record, answering in a sixth of
// the probe timeout - looked dead on about one launch in seven, and the launcher
// then forwarded the hostname and logged "no address answered". Nothing was
// wrong with the timeout: the packet simply never came back.
//
// Two changes follow from that. Probing walks the pool `attempts` times instead
// of once, which drops a false "dead" from ~1/7 to ~1/350 for that server. And a
// proven address is written to a cache file (see host_cache.hpp), so it can be
// tried first next launch, trusted outright for a short while, and used as the
// fallback when a probe proves nothing - because "it answered a minute ago and
// is quiet now" is far more likely to be a dropped datagram than a dead server.
#pragma once

#include <optional>
#include <string>
#include <vector>

class Logger;

namespace hostres {

// One resolved candidate and what the probe made of it.
struct Candidate {
    std::wstring ip;
    bool answered = false;
    unsigned rtt_ms = 0;
    unsigned attempts = 0;   // datagrams sent before it answered or was given up on
};

// Where the address handed to GTA came from, for the log line and for tests.
enum class Source {
    None,      // nothing decided; forward the -h value untouched
    Probe,     // proven this launch
    Pinned,    // a "pin" line in the cache file; used verbatim
    Fresh,     // cached proof young enough to trust without touching the network
    Fallback,  // nothing answered, but a recent proof exists - better than nothing
};

struct Result {
    bool attempted = false;              // false => nothing to do (IP literal, no -h, ...)
    std::wstring original;               // the -h value as given
    std::wstring chosen;                 // what -h should become (empty => leave alone)
    Source source = Source::None;
    std::vector<Candidate> candidates;   // in probe order, for logging
};

// Probe timings and cache policy, set from measurement rather than taste.
//
// per_probe_ms: the first draft used 200 ms, on the theory that a game server
// should answer a 4-byte UDP query promptly. Measured against two real servers:
// s2.gta-mp.cz replies in ~117 ms, but openmp.pgaming.net takes ~230 ms - so
// 200 ms declared a perfectly healthy server dead. 700 ms leaves headroom for an
// intercontinental round trip. It is a CEILING, not a cost: the common case is
// the first address answering, so the usual price is one RTT (~120-250 ms).
//
// attempts: how many times the whole candidate list is walked. Round 1 is the
// old behaviour and costs nothing extra when an address answers; the later
// rounds only ever run when everything looked dead, which is exactly the case
// that measurement showed to be a lie about one time in seven.
//
// total_budget_ms: checked BEFORE each probe, so the true worst case is
// total_budget_ms + per_probe_ms (~3.2 s) - the last probe is allowed to finish
// rather than being cut off mid-flight. A single-address host therefore gets its
// three attempts (2.1 s) in full, while an eight-address dead pool stops early.
//
// cache_trust_s: a proof younger than this is used with no DNS and no probe at
// all. Five minutes covers the case this is for - relaunching the server you
// were just on, typically after a crash - without holding an address so long
// that a genuine server move goes unnoticed.
//
// cache_ttl_s: how old a proof may be and still be used as the fallback when
// this launch's probes prove nothing. A day; the address is only reached for
// after the network has already failed to say otherwise.
//
// max_fallbacks: consecutive fallback uses with no fresh proof in between before
// the entry is dropped. Three keeps a genuinely dead address from being retried
// forever, while riding out a run of lost datagrams.
struct Options {
    unsigned per_probe_ms = 700;      // wait for one candidate's reply
    unsigned total_budget_ms = 2500;  // hard ceiling across all probes
    unsigned max_candidates = 8;      // stop resolving past this many A records
    unsigned attempts = 3;            // times the candidate list is walked
    unsigned cache_trust_s = 300;     // proof this young is used without probing
    unsigned cache_ttl_s = 86400;     // proof this young may serve as a fallback
    unsigned max_fallbacks = 3;       // consecutive unproven uses before forgetting
    std::wstring cache_path;          // "" => do not read or write a cache
};

// Finds "-h <host>" in `args` (also accepting "-h<host>"), and if the value is a
// hostname rather than an IP literal, resolves it and probes each address with a
// SA:MP query on the port from "-p". Returns what it decided; the caller applies
// it. Never throws, and never reports a candidate it did not hear from.
Result resolve_host_arg(const std::vector<std::wstring>& args, Logger& log,
                        const Options& opts = {});

// Applies a Result to an argument vector, replacing the -h value in place.
// A Result with an empty `chosen` leaves `args` untouched, which is the
// fallback whenever the probe proves nothing and no cached address is available
// - passing the hostname through unchanged can only ever be as good as the old
// behaviour, never worse.
void apply(std::vector<std::wstring>& args, const Result& r);

} // namespace hostres
