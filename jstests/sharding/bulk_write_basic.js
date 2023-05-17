/*
 * Tests that bulk write ordered operations succeed on a two shard cluster with both
 * sharded and unsharded data.
 * @tags: [multiversion_incompatible, featureFlagBulkWriteCommand]
 */

(function() {
'use strict';

load("jstests/libs/namespace_utils.js");  // getDBNameAndCollNameFromFullNamespace()

const st = new ShardingTest({
    shards: 2,
    mongos: 2,
    config: 1,
    rs: {nodes: 1},
    mongosOptions: {setParameter: {logComponentVerbosity: tojson({sharding: 4})}}
});

function getCollection(ns) {
    const [dbName, collName] = getDBNameAndCollNameFromFullNamespace(ns);
    return st.s0.getDB(dbName)[collName];
}

const banana = "test.banana";
const orange = "test2.orange";

const staleConfigBananaLog = /7279201.*Noting stale config response.*banana/;
const staleConfigOrangeLog = /7279201.*Noting stale config response.*orange/;
const staleDbTest2Log = /7279202.*Noting stale database response.*test2/;

jsTestLog("Case 1: Collection does't exist yet.");
// Case 1: The collection doesn't exist yet. This results in a StaleConfig error on the
// shards and consequently mongos and the shards must all refresh. Then mongos needs to
// retry the bulk operation.

// Connect via the first mongos. We do this so that the second mongos remains unused until
// a later test case.
const db_s0 = st.s0.getDB("test");
assert.commandWorked(db_s0.adminCommand({
    bulkWrite: 1,
    ops: [{insert: 0, document: {a: 0}}, {insert: 0, document: {a: 1}}],
    nsInfo: [{ns: banana}]
}));

let insertedDocs = getCollection(banana).find({}).toArray();
assert.eq(2, insertedDocs.length, `Inserted docs: '${tojson(insertedDocs)}'`);
assert(checkLog.checkContainsOnce(st.s0, staleConfigBananaLog));

jsTestLog("Case 2: The collection exists for some of writes, but not for others.");
assert.commandWorked(db_s0.adminCommand({
    bulkWrite: 1,
    ops: [
        {insert: 0, document: {a: 2}},
        {insert: 1, document: {a: 0}},
        {insert: 0, document: {a: 3}}
    ],
    nsInfo: [{ns: banana}, {ns: orange}]
}));

insertedDocs = getCollection(banana).find({}).toArray();
assert.eq(4, insertedDocs.length, `Inserted docs: '${tojson(insertedDocs)}'`);
insertedDocs = getCollection(orange).find({}).toArray();
assert.eq(1, insertedDocs.length, `Inserted docs: '${tojson(insertedDocs)}'`);
assert(checkLog.checkContainsOnce(st.s0, staleConfigOrangeLog));

jsTestLog("Case 3: StaleDbVersion when unsharded collection moves between shards.");
const db_s1 = st.s1.getDB("test");
// Case 3: Move the 'test2' DB back and forth across shards. This will result in bulkWrite
// getting a StaleDbVersion error. We run this on s1 so s0 doesn't know about the change.
assert.commandWorked(db_s1.adminCommand({movePrimary: 'test2', to: st.shard0.shardName}));
assert.commandWorked(db_s1.adminCommand({movePrimary: 'test2', to: st.shard1.shardName}));

// Now run the bulk write command on s0.
assert.commandWorked(db_s0.adminCommand(
    {bulkWrite: 1, ops: [{insert: 0, document: {a: 3}}], nsInfo: [{ns: orange}]}));
insertedDocs = getCollection(orange).find({}).toArray();
assert.eq(2, insertedDocs.length, `Inserted docs: '${tojson(insertedDocs)}'`);
assert(checkLog.checkContainsOnce(st.s0, staleDbTest2Log));

jsTestLog("Case 4: The collection is sharded and lives on both shards.");
// Case 4: Shard the collection and manually move chunks so that they live on
// both shards. We stop the balancer as well. We do all of this on s0, but then,
// we run a bulk write command through the s1 that has a stale view of the cluster.
assert.commandWorked(st.stopBalancer());

jsTestLog("Shard the collection.");
assert.commandWorked(getCollection(banana).createIndex({a: 1}));
assert.commandWorked(db_s0.adminCommand({enableSharding: "test"}));
assert.commandWorked(db_s0.adminCommand({shardCollection: banana, key: {a: 1}}));

jsTestLog("Create chunks, then move them.");
assert.commandWorked(db_s0.adminCommand({split: banana, middle: {a: 2}}));
assert.commandWorked(
    db_s0.adminCommand({moveChunk: banana, find: {a: 0}, to: st.shard0.shardName}));
assert.commandWorked(
    db_s0.adminCommand({moveChunk: banana, find: {a: 3}, to: st.shard1.shardName}));

jsTestLog("Running bulk write command.");
assert.commandWorked(db_s1.adminCommand({
    bulkWrite: 1,
    ops: [
        {insert: 0, document: {a: -1}},
        {insert: 1, document: {a: 1}},
        {insert: 0, document: {a: 4}}
    ],
    nsInfo: [{ns: banana}, {ns: orange}]
}));

insertedDocs = getCollection(banana).find({}).toArray();
assert.eq(6, insertedDocs.length, `Inserted docs: '${tojson(insertedDocs)}'`);
insertedDocs = getCollection(orange).find({}).toArray();
assert.eq(3, insertedDocs.length, `Inserted docs: '${tojson(insertedDocs)}'`);

// Checklog doesn't work in this case because mongos may refresh its routing info before
// runningthe bulkWrite command, which means that the logs we're looking for won't get printed.
// However, since the number of documents matched up in the asserts above, it means that mongos
// must've correctly routed the bulkWrite command.

st.stop();
})();
