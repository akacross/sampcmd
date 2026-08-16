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
};

struct Result {
    bool attempted = false;              // false => nothing to do (IP literal, no -h, ...)
    std::wstring original;               // the -h value as given
    std::wstring chosen;                 // what -h should become (empty => leave alone)
    std::vector<Candidate> candidates;   // in resolver order, for logging
};

// Probe timings, set from measurement rather than taste.
//
// The first draft used 200 ms, on the theory that a game server should answer a
// 4-byte UDP query promptly. Measured against two real servers: s2.gta-mp.cz
// replies in ~117 ms, but openmp.pgaming.net takes ~230 ms - so 200 ms declared
// a perfectly healthy server dead. It would have failed safe (the fallback is to
// forward the hostname untouched) but the feature would have been useless for
// anyone not sitting next to their server.
//
// 700 ms leaves headroom for an intercontinental round trip. It is a CEILING,
// not a cost: the common case is the first address answering, so the usual price
// is one RTT (~120-250 ms). Only a pool with dead members ahead of a live one
// pays more, and total_budget_ms bounds that before giving up and forwarding the
// hostname unresolved.
//
// The budget is checked BEFORE each probe, so the true worst case is
// total_budget_ms + per_probe_ms (~3.2 s) - the last probe is allowed to finish
// rather than being cut off mid-flight. Measured: a host whose port has nothing
// listening walks 4 candidates and gives up in ~2.8 s.
struct Options {
    unsigned per_probe_ms = 700;      // wait for one candidate's reply
    unsigned total_budget_ms = 2500;  // hard ceiling across all candidates
    unsigned max_candidates = 8;      // stop resolving past this many A records
};

// Finds "-h <host>" in `args` (also accepting "-h<host>"), and if the value is a
// hostname rather than an IP literal, resolves it and probes each address with a
// SA:MP query on the port from "-p". Returns what it decided; the caller applies
// it. Never throws, and never reports a candidate it did not hear from.
Result resolve_host_arg(const std::vector<std::wstring>& args, Logger& log,
                        const Options& opts = {});

// Applies a Result to an argument vector, replacing the -h value in place.
// A Result with an empty `chosen` leaves `args` untouched, which is the
// fallback whenever the probe proves nothing - passing the hostname through
// unchanged can only ever be as good as the old behaviour, never worse.
void apply(std::vector<std::wstring>& args, const Result& r);

} // namespace hostres
