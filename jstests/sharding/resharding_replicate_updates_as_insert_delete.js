//
// Test to verify that updates that would change the resharding key value are replicated as an
// insert, delete pair.
// @tags: [requires_fcv_47]
//

(function() {
'use strict';

load("jstests/libs/fail_point_util.js");
load('jstests/libs/parallel_shell_helpers.js');
load('jstests/libs/uuid_util.js');

const st = new ShardingTest({mongos: 1, shards: 2});
const dbName = 'test';
const collName = 'foo';
const ns = dbName + '.' + collName;
const mongos = st.s0;

let testDB = mongos.getDB(dbName);
let testColl = testDB.foo;

assert.commandWorked(mongos.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.shardName);
assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {x: 1}}));
assert.commandWorked(mongos.adminCommand({split: ns, middle: {x: 5}}));
// assert.commandWorked(mongos.adminCommand({ moveChunk: ns, find: { x: 5 }, to: st.shard1.shardName
// }));

assert.commandWorked(testColl.insert({_id: 0, x: 2, y: 2}));
let shard0Coll = st.shard0.getCollection(ns);
assert.eq(shard0Coll.find().itcount(), 1);
let shard1Coll = st.shard1.getCollection(ns);
assert.eq(shard1Coll.find().itcount(), 0);

// TODO(SERVER-52620): Remove this simulation section once the reshardCollection command provides
// the needed setup for this test. Simulate resharding operation conditions on donor.
let uuid = getUUIDFromListCollections(testDB, collName);

const tempReshardingColl = "system.resharding." + extractUUIDFromObject(uuid);
const tempReshardingNss = dbName + "." + tempReshardingColl;
assert.commandWorked(testDB.createCollection(tempReshardingColl));
assert.commandWorked(mongos.adminCommand({shardCollection: tempReshardingNss, key: {y: 1}}));
assert.commandWorked(mongos.adminCommand({split: tempReshardingNss, middle: {y: 5}}));
assert.commandWorked(
    mongos.adminCommand({moveChunk: tempReshardingNss, find: {y: 5}, to: st.shard1.shardName}));

jsTestLog("Updating resharding fields");
let donorReshardingFields = {
    "uuid": uuid,
    "state": "initializing",
    "donorFields": {"reshardingKey": {y: 1}}
};
assert.commandWorked(st.configRS.getPrimary().getDB("config").collections.update(
    {_id: ns}, {"$set": {"reshardingFields": donorReshardingFields}}));

assert.commandWorked(mongos.adminCommand({moveChunk: ns, find: {x: 5}, to: st.shard1.shardName}));

jsTestLog("Flushing routing table updates");
assert.commandWorked(st.shard0.adminCommand({_flushDatabaseCacheUpdates: dbName}));
assert.commandWorked(
    st.shard0.adminCommand({_flushRoutingTableCacheUpdates: ns, syncFromConfig: true}));
assert.commandWorked(st.shard1.adminCommand({_flushDatabaseCacheUpdates: dbName}));
assert.commandWorked(
    st.shard1.adminCommand({_flushRoutingTableCacheUpdates: ns, syncFromConfig: true}));

assert.commandWorked(st.shard0.adminCommand(
    {_flushRoutingTableCacheUpdates: tempReshardingNss, syncFromConfig: true}));
assert.commandWorked(st.shard1.adminCommand(
    {_flushRoutingTableCacheUpdates: tempReshardingNss, syncFromConfig: true}));
st.refreshCatalogCacheForNs(mongos, ns);

// TODO(SERVER-52620): Use the actual reshardColleciton command to set up the environment for the
// test. const donor = st.rs0.getPrimary(); const config = st.configRS.getPrimary();

// const failpoint = configureFailPoint(config, "reshardingFieldsInitialized");

// let reshardCollection = (ns, shard) => {
//    jsTestLog("Starting reshardCollection: " + ns + " " + shard);
//    let adminDB = db.getSiblingDB("admin");

//    assert.commandWorked(adminDB.runCommand({
//        reshardCollection: ns,
//        key: { y: 1 }
//    }));

//    jsTestLog("Returned from reshardCollection");
//};

// const awaitShell = startParallelShell(funWithArgs(reshardCollection, ns, st.shard1.shardName),
// st.s.port);

// failpoint.wait();

(() => {
    jsTestLog("Updating doc without a transaction");

    assert.commandFailedWithCode(testColl.update({_id: 0, x: 2}, {$set: {y: 10}}),
                                 ErrorCodes.IllegalOperation);

    jsTestLog("Updated doc");
})();

(() => {
    jsTestLog("Updating doc in a transaction");

    let session = testDB.getMongo().startSession();
    let sessionDB = session.getDatabase(dbName);

    session.startTransaction();
    assert.commandWorked(sessionDB.foo.update({_id: 0, x: 2}, {$set: {y: 10}}));
    session.commitTransaction();

    jsTestLog("Updated doc");

    let donor = st.shard0;
    let donorLocal = donor.getDB('local');
    const ts = session.getOperationTime();

    let donorOplog = donorLocal.oplog.rs.find({ts: {$eq: ts}});
    let oplogEntries = donorOplog.toArray();
    assert.eq(oplogEntries.length, 1);

    // Verify that the applyOps entry contains a delete followed by an insert for the updated
    // collection.
    let applyOps = oplogEntries[0].o.applyOps;
    assert.eq(applyOps.length, 2);
    assert.eq(applyOps[0].op, "d");
    assert.eq(applyOps[0].ns, ns);
    assert.eq(applyOps[1].op, "i");
    assert.eq(applyOps[1].ns, ns);
})();

// TODO(SERVER-52620): Use the actual reshardCollection command to set up the environment for the
// test. failpoint.off(); awaitShell();

st.stop();
})();
