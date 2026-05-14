# SERVER-43218 — `lsidTTLIndex` not refreshed on `localLogicalSessionTimeoutMinutes` change

## Problem

`localLogicalSessionTimeoutMinutes` is declared in `logical_session_cache.idl`
with `set_at: [startup]`. The TTL index on `config.system.sessions` is created
once, at first refresh, by `SessionsCollection::generateCreateIndexesCmd`
(`sessions_collection.cpp:319`), which captures the parameter value as
`localLogicalSessionTimeoutMinutes * 60`. When an operator restarts the cluster
with a new value, freshly created TTL indexes pick it up — but the existing
index is never `collMod`'d. The pre-existing `expireAfterSeconds` silently
overrides the new setting, and sessions continue to expire on the old schedule.

Customer-reported in 2019; deprioritized as "niche" but reproducible whenever an
operator tunes session lifetime (compliance windows, retry budgets, transaction
record lifetime tuning). The cluster never converges to the configured value
without a manual `collMod` per shard.

## Fix path

Three coordinated changes, none requiring new collections:

1. **Promote the parameter to runtime in `logical_session_cache.idl`.** Change
   `set_at: [startup]` to `set_at: [startup, runtime]` and add an
   `on_update: onUpdateLocalLogicalSessionTimeoutMinutes` callback. The
   callback validates the new value (>0, ≤ some operational ceiling) and
   schedules an asynchronous publish — it must not block the `setParameter`
   command. Code site:
   `src/mongo/db/session/logical_session_cache.idl:85-91`.

2. **Config-server publishes; shards converge via `collMod`.** The new
   `on_update` hook, when running on the config server primary, writes the
   target value to a small `config.session_timeout_target` document
   (`{_id: "lsid", expireAfterSeconds: N, generation: M, updatedAt: ts}`).
   Each shard primary watches that document via a change stream
   (already wired for other cross-cluster config) and on each change runs the
   existing `SessionsCollection::generateCollModCmd`
   (`sessions_collection.cpp:331`) — that helper is already shaped exactly for
   this update; today it is only called from index-validation paths. Using
   `$merge`-style idempotent application (write the target, shards reconcile)
   keeps the locus on the data plane and avoids fan-out RPCs from the config
   server. Re-applying the same generation is a no-op `collMod`.

3. **Refresh path stops drifting.** `generateCreateIndexesCmd` continues to
   read `localLogicalSessionTimeoutMinutes` at call time, so a freshly created
   index after a parameter change picks up the new value. The existing
   `checkSessionsCollectionExists` invariant
   (`sessions_collection_rs.cpp:181-186`) — which `uassert`s
   `IndexOptionsConflict` if the index disagrees with the parameter — becomes
   the convergence detector: it goes from "always passes" to "passes once the
   change stream has fanned out", and the `assert.soon` in the new jstest
   pins that window.

## Why this shape

The config-server publish + shard-side reconciliation pattern is what the
cluster already uses for every other cluster-wide setting (chunk size,
balancer state). The update locus stays on the data plane: no new RPC paths,
no synchronous fan-out, no operator-visible failure mode if a shard is
transiently unreachable — the change stream redelivers. The `collMod` helper
already exists and is tested; this change wires it to a trigger.

## Out of scope for this PR

- No C++ modified here. The jstest at
  `jstests/sharding/lsid_ttl_recalc_on_param_change.js` is the regression-pin
  and will fail on unpatched binaries at step 3 (`setParameter` refused at
  runtime). A follow-up PR implements (1)–(3) above; this PR documents the
  fix and lands the test.
- `logicalSessionRefreshMillis` could be promoted similarly but is read on
  every refresh tick, so the symptom is bounded by one refresh interval and
  not customer-evidenced.
