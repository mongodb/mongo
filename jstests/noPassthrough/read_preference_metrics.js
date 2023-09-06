/**
 * Tests that read preference metrics are correctly collected on mongod.
 */

// Test that mongod tracks metrics around read preference.
const rst = new ReplSetTest({nodes: 1});

rst.startSet();
rst.initiate();
const primary = rst.getPrimary();

let serverStatus = assert.commandWorked(primary.getDB("admin").runCommand({serverStatus: 1}));
assert.eq(serverStatus.process, "mongod", tojson(serverStatus));
assert(serverStatus.hasOwnProperty("readPreferenceCounters"), tojson(serverStatus));
rst.stopSet();

// Test that mongos omits metrics around read preference, and shard servers include them.
const st = new ShardingTest({shards: 1});

serverStatus = assert.commandWorked(st.s.getDB("admin").runCommand({serverStatus: 1}));
assert.eq(serverStatus.process, "mongos", tojson(serverStatus));
assert(!serverStatus.hasOwnProperty("readPreferenceCounters"), tojson(serverStatus));

serverStatus = assert.commandWorked(st.shard0.getDB("admin").runCommand({serverStatus: 1}));
assert.eq(serverStatus.process, "mongod", tojson(serverStatus));
assert(serverStatus.hasOwnProperty("readPreferenceCounters"), tojson(serverStatus));

st.stop();
