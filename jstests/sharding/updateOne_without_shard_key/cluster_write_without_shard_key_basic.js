/**
 * Tests the internal command _clusterWriteWithoutShardKey.
 *
 * @tags: [requires_fcv_62, featureFlagUpdateOneWithoutShardKey]
 */
(function() {
"use strict";

let st = new ShardingTest({shards: 1, rs: {nodes: 1}});
let dbName = "test";
let mongosConn = st.s.getDB(dbName);
let shardConn = st.shard0.getDB(dbName);
let cmdObj = {_clusterWriteWithoutShardKey: 1, writeCmd: {}, shardId: ""};

assert.commandWorked(mongosConn.runCommand(cmdObj));
assert.commandFailedWithCode(shardConn.runCommand(cmdObj), ErrorCodes.CommandNotFound);

st.stop();
})();
