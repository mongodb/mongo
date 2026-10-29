/**
 * Verifies creating the logical sessions collection TTL index retries on stale version errors.
 *
 * A shard can never be ignorant of its own metadata under authoritative shards, so the staleness
 * cannot be induced on the recipient shard. Instead, the config server resolves the routing table
 * for config.system.sessions and is then paused (via a failpoint) before dispatching the versioned
 * createIndexes; a migration committed through a router during that pause leaves the config server's
 * resolved routing stale, so the dispatch hits a StaleConfig and must refresh and retry.
 *
 * @tags: [
 *   # This test pauses index generation on a specific config server primary via a failpoint and
 *   # asserts on that same node's stale config counter, so it cannot tolerate the config primary
 *   # stepping down mid-scenario.
 *   does_not_support_stepdowns,
 * ]
 */

import {ChunkHelper} from "jstests/concurrency/fsm_workload_helpers/cluster_scalability/chunks.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {
    getShardsWithAndWithoutChunk,
    validateSessionsCollection,
} from "jstests/libs/sessions_collection.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {ShardVersioningUtil} from "jstests/sharding/libs/shard_versioning_util.js";

let st = new ShardingTest({shards: 2});

// Validate the initial state.
const {
    shardWithSessionChunk: shardOriginallyWithChunk,
    shardWithoutSessionChunk: shardOriginallyWithoutChunk,
} = getShardsWithAndWithoutChunk(st, st.shard0, st.shard1);
validateSessionsCollection(
    shardOriginallyWithChunk,
    true /* collectionExists */,
    true /* indexExists */,
);
validateSessionsCollection(shardOriginallyWithoutChunk, false, false);

// Drop the TTL index on the shardOriginallyWithChunk.
assert.commandWorked(
    shardOriginallyWithChunk.getDB("config").system.sessions.dropIndex({lastUse: 1}),
);

// Validate that index has been dropped.
validateSessionsCollection(shardOriginallyWithChunk, true, false);
validateSessionsCollection(shardOriginallyWithoutChunk, false, false);

const configPrimary = st.configRS.getPrimary();
const staleConfigCountStart = ShardVersioningUtil.getRouterStaleConfigErrorCount(configPrimary);

// Pause index generation on the config server after it resolves the routing table but before it
// dispatches the versioned createIndexes.
const fp = configureFailPoint(configPrimary, "hangBeforeGeneratingSessionsCollectionIndexes");

// Trigger the session cache refresh (which recreates the index) in a parallel shell so it blocks on
// the failpoint while we move the chunk.
const refreshJoin = startParallelShell(function () {
    assert.commandWorked(db.getSiblingDB("config").runCommand({refreshLogicalSessionCacheNow: 1}));
}, configPrimary.port);

fp.wait();

// Move the only chunk in the logical sessions collection to shardOriginallyWithoutChunk through a
// router. The config server's already-resolved routing still points at shardOriginallyWithChunk, so
// the createIndexes dispatch will be rejected with a StaleConfig.
ChunkHelper.moveChunk(
    st.s.getDB("config"),
    "system.sessions",
    [{_id: MinKey}, {_id: MaxKey}],
    shardOriginallyWithoutChunk.shardName,
    true /* waitForDelete */,
);

fp.off();
refreshJoin();

// Verify that the refresh recreated the index only on the shard that owns the logical sessions
// collection chunk, after the config server hit a StaleConfig on the previous owner and retried.
validateSessionsCollection(shardOriginallyWithChunk, true, false);
validateSessionsCollection(shardOriginallyWithoutChunk, true, true);
assert.gt(
    ShardVersioningUtil.getRouterStaleConfigErrorCount(configPrimary),
    staleConfigCountStart,
    "expected the config server to observe a StaleConfig and retry index creation",
);

st.stop();
