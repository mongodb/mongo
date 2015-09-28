// Tests operation of the cluster when the config servers have no primary and thus the cluster
// metadata is in read-only mode.
var st = new ShardingTest({shards: 1});

// Create the "test" database while the cluster metadata is still writeable.
st.s.getDB('test').foo.insert({a:1});

// Take down two of the config servers so the remaining one goes into SECONDARY state.
st.configRS.stop(0);
st.configRS.stop(1);
assert.throws(function() {st.configRS.getMaster(5000);});

jsTestLog("Starting a new mongos when the config servers have no primary which should work");
var mongos2 = MongoRunner.runMongos({configdb: st.configRS.getURL()});
assert.neq(null, mongos2);

jsTestLog("Doing ops that don't require metadata writes and thus should succeed");
assert.writeOK(mongos2.getDB('test').foo.insert({a:1}));
assert.eq(2, mongos2.getDB('test').foo.count());

assert.throws(function() {st.s.getDB('config').shards.findOne();});
st.s.setSlaveOk(true);
var shardDoc = st.s.getDB('config').shards.findOne();
assert.neq(null, shardDoc);

jsTestLog("Doing ops that require metadata writes and thus should fail")
assert.writeError(st.s.getDB("newDB").foo.insert({a:1}));
assert.commandFailed(st.s.getDB('admin').runCommand({shardCollection: "test.foo", key: {a:1}}));

st.stop();