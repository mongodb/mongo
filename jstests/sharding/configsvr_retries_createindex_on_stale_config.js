/**
 * Verifies creating the logical sessions collection TTL index retries on stale version errors.
 */

import {
    getShardsWithAndWithoutChunk,
    validateSessionsCollection
} from "jstests/libs/sessions_collection.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {ShardVersioningUtil} from "jstests/sharding/libs/shard_versioning_util.js";

let st = new ShardingTest({shards: 2});

// Validate the initial state.
const {
    shardWithSessionChunk: shardOriginallyWithChunk,
    shardWithoutSessionChunk: shardOriginallyWithoutChunk
} = getShardsWithAndWithoutChunk(st, st.shard0, st.shard1);
validateSessionsCollection(
    shardOriginallyWithChunk, true /* collectionExists */, true /* indexExists */);
validateSessionsCollection(shardOriginallyWithoutChunk, false, false);

// Drop the TTL index on the shardOriginallyWithChunk.
assert.commandWorked(
    shardOriginallyWithChunk.getDB("config").system.sessions.dropIndex({lastUse: 1}));

// Validate that index has been dropped.
validateSessionsCollection(shardOriginallyWithChunk, true, false);
validateSessionsCollection(shardOriginallyWithoutChunk, false, false);

// Move the only chunk in the logical sessions collection from shardOriginallyWithChunk to
// shardOriginallyWithoutChunk with refresh suppressed.
ShardVersioningUtil.moveChunkNotRefreshRecipient(st.s,
                                                 "config.system.sessions",
                                                 shardOriginallyWithChunk,
                                                 shardOriginallyWithoutChunk,
                                                 {_id: MinKey});

// Refresh session cache.
assert.commandWorked(
    st.configRS.getPrimary().getDB("config").runCommand({refreshLogicalSessionCacheNow: 1}));

// Verify that the refresh recreated the index only on the shard that owns the logical sessions
// collection chunk despite that shard being stale.
validateSessionsCollection(shardOriginallyWithChunk, true, false);
validateSessionsCollection(shardOriginallyWithoutChunk, true, true);

st.stop();
