// Tests that a movePrimary will fail if the database doesn't have a version in config.databases.

// Do not check metadata consistency as the database version is missing for testing purposes.
TestData.skipCheckMetadataConsistency = true;
TestData.skipCheckRoutingTableConsistency = true;

(function() {
"use strict";

const dbName = "test";

const st = new ShardingTest({shards: 2});

assert.commandWorked(st.s.getDB("config").getCollection("databases").insert({
    _id: dbName,
    primary: st.shard0.shardName
}));

assert.commandFailed(st.s.adminCommand({movePrimary: dbName, to: st.shard1.shardName}));

st.stop();
})();
