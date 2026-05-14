# Automatic Discovery of SLS Topology

**Ticket:** SERVER-111844
**Team:** Storage Engines - Server Integration (sesi-disagg)
**Status:** Design

## Problem

SLS (Storage Layer Services) topology -- the set of peers, cells, and log servers in a
disaggregated cluster -- is currently fully static in the disagg config. When the spec drifts
from the live set (e.g. a Kubernetes StatefulSet rolls a pod), reads and writes target stale
endpoints, producing connection storms. Much of this information is already available at
runtime via the Kubernetes headless service or peer-to-peer gossip; learn it rather than
hard-code it.

## Approach

Replace the static config with a **bootstrap-set + gossip** model.

### Bootstrap

Each disagg-aware process accepts a small bootstrap set from the config server (or from a
Kubernetes headless service DNS query under k8s). Seeds carry a config-server signature so a
discoverer can verify authorization before admission. Bootstrap is the only externally-supplied
input; everything else is learned.

### Per-cell heartbeat + reputation

Cell members exchange heartbeats every `heartbeatPeriodMs` (default 500ms), each carrying the
sender's view of cell membership. Each peer maintains a per-neighbour reputation score (EWMA of
heartbeat freshness + probe RTT); a peer below `reputationFloor` is demoted from `known` to
`pending` and re-probed before being used for steering.

### Topology cache TTL + refresh policy

The cache has TTL `topologyTtlSec` (default 30s). On expiry the discoverer:

1. Re-queries the bootstrap source.
2. Runs `GossipPull` against each reachable known peer.
3. Drains `pending` through the authorization gate (config-server signature check).
4. Expires any peer no longer in `Authorized` and not in `BootstrapSet`.

Refresh is also triggered eagerly on a connection failure to any peer in `known`.

### Authorization gate

Every peer entering `pending` -- regardless of source -- is verified against the config
server's signed allow-list before promotion to `known`. This is the single safety-critical
step; unauthorized peers are dropped, never cached. The TLA+ spec proves this as
`EveryDiscoveredPeerIsAuthorized` and convergence as `EveryReachablePeerEventuallyDiscovered`.

## Failure modes

**Split-view across discoverers.** Two discoverers may transiently disagree. Writes route via
the config-server-issued routing table (already versioned), not via local discoverer state.
Local discovery only feeds heartbeat targets and read fanout; both tolerate transient
over-fanout (bounded `2x` amplification) and under-fanout (retried with refreshed view).

**Stale-config detection.** Two signals force a synchronous refresh: (a) heartbeats reporting a
`topologyEpoch` strictly greater than the cached epoch, (b) authorization-gate rejections on
peers that previously passed (allow-list rotated).

**Bootstrap source unavailable.** If unreachable at startup, the process refuses to serve
traffic and exits non-zero after `bootstrapTimeoutSec` (default 30s). Continuing with no seeds
defeats the safety property.

**Poisoned gossip.** The spec models `InjectRumor` to confirm the gate rejects unauthorized
peers even when they propagate via gossip; production drops rumors after the same TTL.

## Rollout

Phase 1: shadow mode (compare discovered vs static, log divergences). Phase 2: discovered
topology drives read fanout. Phase 3: drives write steering once divergence drops below 0.1%.
