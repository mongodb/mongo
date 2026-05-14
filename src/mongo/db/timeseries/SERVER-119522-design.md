# SERVER-119522 — RoutingContext must refresh time-series buckets cache

## Problem

`RoutingContext` on `mongos` classifies a namespace as a time-series collection by consulting the
local routing cache for the underlying `system.buckets.<name>` namespace. When that cache entry is
stale — typically right after a `dropDatabase` + `createCollection(timeseries)` +
`shardCollection` sequence executed through a *different* `mongos`, or after a shard transition
(`addShard` / `removeShard`) — the stale router can:

1. Conclude that `<db>.<coll>` is "non-existent" or "UNSHARDED" and route a retryable update to the
   dbPrimary with `shardVersion=UNSHARDED`.
2. Cause the destination shard to execute the write through
   `write_ops_exec::runTimeseriesRetryableUpdates`, which uses a local (non-cluster) TransactionAPI
   executor and throws the internal error `6638800` instead of a `StaleConfig`.
3. Persist the misclassification until some unrelated operation causes a refresh.

## Fix path

Refresh the `system.buckets.<name>` cache entry as part of `RoutingContext` initialization for any
namespace that may correspond to a time-series collection, rather than only on a `StaleConfig`
rebound from a shard.

Concretely:

- During `RoutingContext::initFor(NamespaceString nss)`, after the primary
  `CollectionRoutingInfo` for `nss` is resolved, additionally resolve the routing info for the
  paired `nss.makeTimeseriesBucketsNamespace()`.
- If the buckets entry exists and is sharded but the local cache is older than the configsvr
  topology version observed during this `RoutingContext` init, force a single targeted refresh of
  the buckets entry *before* the routing decision is finalized.
- Cache the paired (`nss`, `bucketsNss`) routing-info tuple on the `RoutingContext` so downstream
  targeting (in particular `CollectionRoutingInfoTargeter`) sees a self-consistent view: either
  both entries are fresh, or both are flagged stale and a `StaleConfig` is surfaced cleanly.
- Make the shard side defensive: if `write_ops_exec::runTimeseriesRetryableUpdates` discovers that
  the router targeted with `shardVersion=UNSHARDED` against a collection that is in fact a sharded
  time-series collection, throw `StaleConfig` instead of `6638800` so the router can refresh and
  re-plan.

## Correctness argument

The pairing rule (`nss` ↔ `bucketsNss`) is already an invariant elsewhere in the catalog. Pulling
that invariant into `RoutingContext` init closes the window during which the two cache entries can
diverge, which is exactly the window the bug exploited. The shard-side `StaleConfig` upgrade
preserves backwards compatibility — older routers still receive a routing error they know how to
handle — and removes the persistent-failure mode where one bad classification can outlive every
subsequent operation until something else perturbs the cache.

## Test coverage

`jstests/sharding/timeseries/routing_context_identifies_ts_after_cache_invalidation.js` reproduces
the scenario: two mongos, time-series creation via one, cache priming on the other, an
addShard/removeShard cycle to perturb topology, and an aggregation through the stale router that
must round-trip cleanly to the buckets collection.
