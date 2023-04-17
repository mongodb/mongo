/*
 * Ensures that if the primary config server is blackholed from the point of view of mongos, CRUD
 * and read-only config operations continue to work.
 *
 * @tags: [config_shard_incompatible]
 */

(function() {
'use strict';

load('jstests/replsets/rslib.js');

// Checking index consistency involves talking to the primary config server which is blackholed from
// the mongos in this test.
TestData.skipCheckingIndexesConsistentAcrossCluster = true;
TestData.skipCheckOrphans = true;
TestData.skipCheckShardFilteringMetadata = true;

var st = new ShardingTest({shards: 2, mongos: 1, useBridge: true, config: 3});

var testDB = st.s.getDB('BlackHoleDB');
var configDB = st.s.getDB('config');

assert.commandWorked(testDB.adminCommand({enableSharding: 'BlackHoleDB'}));
assert.commandWorked(
    testDB.adminCommand({shardCollection: testDB.ShardedColl.getFullName(), key: {_id: 1}}));

var bulk = testDB.ShardedColl.initializeUnorderedBulkOp();
for (var i = 0; i < 1000; i++) {
    bulk.insert({_id: i});
}
assert.commandWorked(bulk.execute());

const configPrimary = st.configRS.getPrimary();
const admin = configPrimary.getDB("admin");

// Set the priority and votes to 0 for secondary config servers so that in the case
// of an election, they cannot step up. If a different node were to step up, the
// config server would no longer be blackholed from mongos.
let conf = admin.runCommand({replSetGetConfig: 1}).config;
for (let i = 0; i < conf.members.length; i++) {
    if (conf.members[i].host !== configPrimary.host) {
        conf.members[i].votes = 0;
        conf.members[i].priority = 0;
    }
}
reconfig(st.configRS, conf);
jsTest.log('Partitioning the config server primary from the mongos');
configPrimary.discardMessagesFrom(st.s, 1.0);
st.s.discardMessagesFrom(configPrimary, 1.0);

assert.commandWorked(testDB.adminCommand({flushRouterConfig: 1}));

// This should fail, because the primary is not available
jsTest.log('Doing write operation on a new database and collection');
assert.writeError(
    st.s.getDB('NonExistentDB')
        .TestColl.insert({_id: 0, value: 'This value will never be inserted'}, {maxTimeMS: 15000}));

jsTest.log('Doing CRUD operations on the sharded collection');
assert.soon(() => {
    // eventually the mongos will route the traffic to a secondary of the config replica set
    try {
        return testDB.ShardedColl.countDocuments({}) === 1000;
    } catch (err) {
        jsTest.log("Attempt to count documents failed with error: " + err);
    }
    return false;
});
assert.commandWorked(testDB.ShardedColl.insert({_id: 1000}));
assert.eq(1001, testDB.ShardedColl.find().count());

jsTest.log('Doing read operations on a config server collection');

// Should fail due to primary read preference
assert.throws(function() {
    configDB.chunks.find().itcount();
});
assert.throws(function() {
    configDB.chunks.find().count();
});
assert.throws(function() {
    configDB.chunks.aggregate().itcount();
});

// With secondary read pref config server reads should work
st.s.setReadPref('secondary');
assert.lt(0, configDB.chunks.find().itcount());
assert.lt(0, configDB.chunks.find().count());
assert.lt(0, configDB.chunks.aggregate().itcount());

jsTest.log('Remove network partition before tearing down');
configPrimary.discardMessagesFrom(st.s, 0.0);
st.s.discardMessagesFrom(configPrimary, 0.0);

st.stop();
}());
