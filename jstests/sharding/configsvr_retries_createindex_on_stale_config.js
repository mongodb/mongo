/**
 * Verifies creating the logical sessions collection TTL index retries on stale version errors.
 */

(function() {
"use strict";

load('jstests/libs/sessions_collection.js');
load("jstests/sharding/libs/shard_versioning_util.js");
load("jstests/libs/fail_point_util.js");

let st = new ShardingTest({shards: 2});

// Validate the initial state.
validateSessionsCollection(st.shard0, true, true);
validateSessionsCollection(st.shard1, false, false);
validateSessionsCollection(st.configRS.getPrimary(), TestData.configShard, TestData.configShard);

// Drop the TTL index on shard0.
assert.commandWorked(st.shard0.getDB("config").system.sessions.dropIndex({lastUse: 1}));

// Validate that index has been dropped.
validateSessionsCollection(st.shard0, true, false);
validateSessionsCollection(st.shard1, false, false);

// Move the only chunk in the logical sessions collection from shard0 to shard1 with refresh
// suppressed.
ShardVersioningUtil.moveChunkNotRefreshRecipient(
    st.s, "config.system.sessions", st.shard0, st.shard1, {_id: MinKey});

// Refresh session cache.
assert.commandWorked(
    st.configRS.getPrimary().getDB("config").runCommand({refreshLogicalSessionCacheNow: 1}));

// Verify that the refresh recreated the index only on the shard that owns the logical sessions
// collection chunk despite that shard being stale.
validateSessionsCollection(st.shard0, true, false);
validateSessionsCollection(st.shard1, true, true);

st.stop();
})();
