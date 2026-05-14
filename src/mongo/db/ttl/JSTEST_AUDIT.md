# TTL jstest Audit (SERVER-97661)

Scope: catalog the existing `jstests/**/ttl*` and related suites, classify
each by *what it actually needs from a live server*, identify which ones
are candidates for conversion to C++ unit tests under
`src/mongo/db/ttl/ttl_test.cpp` (already 1272 lines, 25+ `TEST_F`s), and
list coverage gaps fillable by new jstests where unit conversion is not
feasible.

Branch: `substrate-contrib/w3-88`. Dependency: SERVER-96179 (further
extension to ttl unit testing) - blocked, separate work.

## Inventory (37 jstests touching TTL)

### Tier A - Strong unit-test conversion candidates

Tests whose load-bearing behaviour is pure `TTLMonitor::doTTLPass()`
mechanics, validated through `serverStatus().metrics.ttl.passes` plus
`configureFailPoint` choreography. The pass loop, sub-pass accounting,
and per-collection delete plan are already covered by `TTLTest::...`
fixtures in `ttl_test.cpp`. Each row below either *can* be replaced
outright or *can* shed its failpoint half:

| jstest | Failpoints in use | Unit-test equivalent |
|---|---|---|
| `noPassthrough/ttl/ttl_changes_are_immediate.js` | none, but loops on `metrics.ttl.passes` | `TTLPassSingleCollectionTwoIndexes` already covers two-index immediacy; extend with a `collMod` step |
| `noPassthrough/ttl/ttl_multiple_indexes_and_collections.js` | none | `TTLPassMultipCollectionsPass` covers the matrix; jstest is a slower duplicate |
| `noPassthrough/ttl/ttl_partial_index.js` | none | Add `TTLPassPartialIndex` to `ttl_test.cpp` (gap; SERVER-17984 only has the js) |
| `noPassthrough/ttl/ttl_hidden_index.js` | none | Add `TTLPassHiddenIndexSkipped` to `ttl_test.cpp` |
| `noPassthrough/ttl/ttl_with_dropIndex.js` | none | `TTLSubPassesStartRemovingFromNewTTLIndex` covers the inverse; add the drop direction |
| `noPassthrough/ttl/ttlMonitorSleepSecs_parameter.js` | none | Already partially covered by `TTLRunMonitorThread`; add a `onUpdateTTLMonitorSleepSeconds` setter test |
| `noPassthrough/ttl/ttl_expire_nan_warning_on_startup.js` | `skipTTLIndexValidationOnCreateIndex`, `skipTTLIndexExpireAfterSecondsValidation` | The failpoint exists *only* to inject an otherwise-impossible catalog row. A unit test seeding the catalog directly (see `SkipInvalidTTLTest`) removes both failpoints |
| `noPassthrough/ttl/ttl_non_int_expire_after_seconds.js` | `skipTTLIndexExpireAfterSecondsValidation` | Same shape: `SkipInvalidTTLTest::TTLNonNumericExpireAfterSeconds` already exists; the jstest is redundant once the startup-warning path is unit-tested |

### Tier B - Keep as jstest, but trim failpoint dependence

Tests where the *behaviour under stepdown / write-block / cross-shard*
genuinely requires a running server, but the failpoint usage is a
shortcut around a race that a unit test could side-step:

| jstest | Failpoints | Why keep | Cleanup |
|---|---|---|---|
| `replsets/kill_ttl_on_stepdown.js` | `hangTTLMonitorWithLock` | True replication-layer behaviour: interrupt + thread survival | Failpoint cannot be removed without splitting the test |
| `noPassthrough/ttl/user_write_blocking_ttl_index.js` | `hangTTLMonitorBetweenPasses` | UserWriteBlockMode interaction is server-wide | Use `TTLUtil.waitForPass` instead of failpoint-gated pass counting; failpoint stays only for the global write-block window |
| `noPassthrough/ttl/ttl_batch_deletes.js` | (none in this file; mentions `ttlMonitorBatchDeletes`) | Batched-delete pathway lives in `query/internal_plans.cpp` | Already failpoint-free; keep |

### Tier C - Sharding / Resharding suites - keep as jstest

Cross-shard orchestration is out of reach for the unit harness:

- `sharding/ttl_sharded.js`
- `sharding/ttl_monitor_recovers_shard_version.js`
- `sharding/ttl_deletes_not_targeting_orphaned_documents.js`
- `sharding/analyze_shard_key/ttl_delete_samples.js`
- `sharding/resharding_timeseries/reshard_timeseries_ttl_deletes.js`
- `noPassthrough/ttl/ttl_resharding_collection.js`

### Tier D - Adjacent suites referenced for completeness

- `core/ddl/ttl_index_options.js`, `core/ddl/coll_mod_convert_to_ttl.js` (catalog DDL; not TTLMonitor)
- `noPassthrough/clustered_collections/clustered_collection_ttl.js` (clustered-collection reaping)
- `noPassthrough/timeseries/ttl/{timeseries_ttl,timeseries_expire,timeseries_expires_with_partial_index}.js`
- `noPassthrough/catalog/coll_mod_ttl.js`
- `noPassthroughWithMongod/ttl/{ttl1,ttl_repl,ttl_repl_maintenance,ttl_repl_secondary_disabled}.js`
- `noPassthroughWithMongod/capped/ttl_index_capped_collection.js`
- `replsets/ttl_index_with_capped_collection.js`
- `noPassthrough/admission/execution_control/ttl_monitor_deprioritization.js`
- `noPassthrough/query/change_streams/change_stream_pre_images_with_ttl.js`
- `concurrency/fsm_workloads/timeseries/insert_ttl_{timeseries,retry_writes_timeseries}.js`
- `concurrency/fsm_workloads/crud/indexed_insert/indexed_insert_ttl.js`
- `core/timeseries/ddl/timeseries_index_ttl_partial.js`

## Coverage gaps surfaced during audit

1. **Hidden TTL index never reaps**. `ttl_hidden_index.js` exists but
   has no failpoint scaffolding *and* no negative-path coverage in
   `ttl_test.cpp`. If a future change inverts the hidden-index check in
   `TTLCollectionCache`, only the jstest catches it - and only the
   `noPassthrough` variant runs it. Worth a unit test mirror; jstest
   still useful as an end-to-end pin.

2. **`collMod` of `expireAfterSeconds` while a pass is in flight**. No
   existing jstest exercises the case where `expireAfterSeconds` is
   mutated mid-pass for an index that is *currently* being scanned by
   the TTL monitor. Race with `TTLCollectionCache::updateExpireAfter`.
   Failpoint-driven; needs jstest, not unit test.

3. **TTL monitor restart after `dropDatabase` of every TTL-bearing
   db**. `ttl_with_dropIndex.js` covers a single index drop, but not
   the database-wide cleanup that purges every `TTLInfoMap` entry. The
   recent fix referenced in `aa2afa865d` (SERVER-97657, "fix ttl info
   map on index update") makes this load-bearing.

## Concrete deliverables in this branch

New jstests landed alongside this audit (do *not* push, do *not*
modify `skill-graph-mcp`):

### `jstests/noPassthrough/ttl/ttl_collMod_expire_during_pass.js`

Exercises gap 2. Uses `hangTTLMonitorBetweenPasses` to freeze a pass
mid-flight, fires a `collMod` that flips `expireAfterSeconds` from
`60000` down to `0`, releases the failpoint, and asserts that the next
two TTL passes pick up the new expiration. Builds on `TTLUtil.waitForPass`
rather than rolling its own pass-count loop, so the only failpoint is
the hang used to create the race window deterministically.

### `jstests/noPassthrough/ttl/ttl_dropDatabase_clears_info_map.js`

Exercises gap 3. Creates two databases, each with two TTL indexes;
waits for the TTL info map to reflect 4 entries via
`serverStatus().metrics.ttl.deletedDocuments` after a forced pass;
issues `dropDatabase` against one db; uses
`hangTTLMonitorBetweenPasses` to single-step a pass; asserts the
remaining 2 entries reap as expected and that no log-line for the
dropped collection re-surfaces (would indicate a stale `TTLInfoMap`
entry).

## Recommended follow-up tickets

- **SERVER-XXXXX (unit)**: convert Tier-A rows to `ttl_test.cpp`
  fixtures (`TTLPassPartialIndex`, `TTLPassHiddenIndexSkipped`,
  `TTLPassDropIndexClearsEntry`, `TTLMonitorSleepSecsParameter`).
  Drops 4 `noPassthrough` jstests; saves >3min of CI wall.
- **SERVER-XXXXX (failpoint removal)**: once the above lands, retire
  `skipTTLIndexValidationOnCreateIndex` + `skipTTLIndexExpireAfterSecondsValidation`
  - they exist *only* to feed `ttl_expire_nan_warning_on_startup.js`
  and `ttl_non_int_expire_after_seconds.js`. Unit equivalents seed the
  catalog directly via `SkipInvalidTTLTest`.
- **SERVER-XXXXX (jstest)**: write a sharded counterpart of
  `ttl_dropDatabase_clears_info_map.js` covering the shard-version
  recovery interaction surfaced by `ttl_monitor_recovers_shard_version.js`.
