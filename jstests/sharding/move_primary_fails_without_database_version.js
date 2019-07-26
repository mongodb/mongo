// Tests that a movePrimary will fail if the database doesn't have a version in config.databases
(function() {
"use strict";

const dbName = "test";

const st = new ShardingTest({shards: 2});

assert.commandWorked(st.s.getDB("config")
                         .getCollection("databases")
                         .insert({_id: dbName, partitioned: false, primary: st.shard0.shardName}));

assert.commandFailedWithCode(st.s.adminCommand({movePrimary: dbName, to: st.shard1.shardName}),
                             ErrorCodes.InternalError);

st.stop();
})();