/**
 * Tests for basic functionality of the unshard collection feature.
 *
 * @tags: [
 *  requires_fcv_80,
 *  featureFlagUnshardCollection,
 *  assumes_balancer_off,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

let st = new ShardingTest({mongos: 1, shards: 2});

// Helper to bulk insert documents with sequential key values in range [start, end).
function bulkInsertDocs(coll, keyField, start, end) {
    const bulkOp = coll.initializeUnorderedBulkOp();
    for (let i = start; i < end; ++i) {
        bulkOp.insert({[keyField]: i});
    }
    const result = bulkOp.execute();
    const count = end - start;
    assert.eq(result.nInserted, count, `Bulk insert: expected ${count} docs, inserted ${result.nInserted}`);
}

function assertNumDocsOnShard(namespace, shardName, expectedCount) {
    const shard = shardName === st.shard0.shardName ? st.rs0 : st.rs1;
    const actualCount = shard.getPrimary().getCollection(namespace).countDocuments({});
    assert.eq(
        actualCount,
        expectedCount,
        `Expected ${expectedCount} documents on shard ${shardName} but found ${actualCount} for ${namespace}`,
    );
}

const dbName = jsTestName();
const collName = "foo";
const ns = dbName + "." + collName;
let mongos = st.s0;
let shard0 = st.shard0.shardName;
let shard1 = st.shard1.shardName;
assert.commandWorked(mongos.adminCommand({enableSharding: dbName, primaryShard: shard0}));

jsTest.log("Verify unshardCollection fails on non-existent collection");
assert.commandFailedWithCode(mongos.adminCommand({unshardCollection: ns}), ErrorCodes.NamespaceNotFound);

jsTest.log("Verify unshardCollection fails on unsharded collection");
const unshardedCollName = "foo_unsharded";
const unshardedCollNS = dbName + "." + unshardedCollName;
assert.commandWorked(st.s.getDB(dbName).runCommand({create: unshardedCollName}));
let res = mongos.adminCommand({unshardCollection: unshardedCollNS});
assert.commandFailedWithCode(res, [ErrorCodes.NamespaceNotFound, ErrorCodes.NamespaceNotSharded]);

jsTest.log("Verify unshardCollection succeeds with explicit toShard option");
let coll = mongos.getDB(dbName)[collName];
assert.commandWorked(coll.createIndex({oldKey: 1}));
assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {oldKey: 1}}));

assert.commandWorked(mongos.adminCommand({split: ns, middle: {oldKey: 0}}));
assert.commandWorked(mongos.adminCommand({moveChunk: ns, find: {oldKey: -1}, to: shard0}));
assert.commandWorked(mongos.adminCommand({moveChunk: ns, find: {oldKey: 10}, to: shard1}));

bulkInsertDocs(coll, "oldKey", -25, 25);
assertNumDocsOnShard(ns, shard1, 25);
assert.commandWorked(mongos.adminCommand({unshardCollection: ns, toShard: shard1}));

// Verify the collection is marked as unsplittable.
let configDb = mongos.getDB("config");
let unshardedColl = configDb.collections.findOne({_id: ns});
assert.eq(unshardedColl.unsplittable, true);

assertNumDocsOnShard(ns, shard1, 50);
assertNumDocsOnShard(ns, shard0, 0);

// Verify there is only one chunk for the collection.
let unshardedChunk = configDb.chunks.find({uuid: unshardedColl.uuid}).toArray();
assert.eq(1, unshardedChunk.length);

jsTest.log("Verify unshardCollection is idempotent with same toShard");
const collInfo = mongos.getDB(dbName).getCollectionInfos({name: coll.getName()})[0];
const prevCollUUID = collInfo.info.uuid;
assert.commandWorked(mongos.adminCommand({unshardCollection: ns, toShard: shard1}));
assert.eq(mongos.getDB(dbName).getCollectionInfos({name: coll.getName()})[0].info.uuid, prevCollUUID);

jsTest.log("Verify unshardCollection fails when trying to move already unsharded collection");
assert.commandFailedWithCode(
    mongos.adminCommand({unshardCollection: ns, toShard: shard0}),
    ErrorCodes.NamespaceNotSharded,
);

jsTest.log("Verify unshardCollection succeeds without explicit toShard option");
const newCollName = "foo1";
const newCollNs = dbName + "." + newCollName;
assert.commandWorked(mongos.adminCommand({shardCollection: newCollNs, key: {oldKey: 1}}));

assert.commandWorked(mongos.adminCommand({split: newCollNs, middle: {oldKey: 0}}));
assert.commandWorked(mongos.adminCommand({moveChunk: newCollNs, find: {oldKey: -1}, to: shard0}));
assert.commandWorked(mongos.adminCommand({moveChunk: newCollNs, find: {oldKey: 10}, to: shard1}));

coll = mongos.getDB(dbName)[newCollName];
bulkInsertDocs(coll, "oldKey", -30, 30);

assert.commandWorked(mongos.adminCommand({unshardCollection: newCollNs}));

const docsOnShard0 = st.rs0.getPrimary().getCollection(newCollNs).countDocuments({});
const docsOnShard1 = st.rs1.getPrimary().getCollection(newCollNs).countDocuments({});
assert(
    docsOnShard0 === 60 || docsOnShard1 === 60,
    `Expected 60 documents on one shard, found ${docsOnShard0} on shard0 and ${docsOnShard1} on shard1`,
);

jsTest.log("Verify unshardCollection fails when called directly on shard");
assert.commandFailedWithCode(
    st.shard0.adminCommand({unshardCollection: ns, toShard: shard1}),
    ErrorCodes.CommandNotFound,
);

jsTest.log("Verify unshardCollection succeeds when shard key is _id");
assert.commandWorked(mongos.adminCommand({shardCollection: newCollNs, key: {_id: 1}}));
assert.commandWorked(mongos.adminCommand({split: newCollNs, middle: {_id: 0}}));
assert.commandWorked(mongos.adminCommand({moveChunk: newCollNs, find: {_id: -1}, to: shard0}));
assert.commandWorked(mongos.adminCommand({moveChunk: newCollNs, find: {_id: 10}, to: shard1}));

coll = mongos.getDB(dbName)[newCollName];
bulkInsertDocs(coll, "_id", -30, 30);

assert.commandWorked(mongos.adminCommand({unshardCollection: newCollNs}));

const finalDocsOnShard0 = st.rs0.getPrimary().getCollection(newCollNs).countDocuments({});
const finalDocsOnShard1 = st.rs1.getPrimary().getCollection(newCollNs).countDocuments({});
assert(
    finalDocsOnShard0 === 120 || finalDocsOnShard1 === 120,
    `Expected 120 documents on one shard, found ${finalDocsOnShard0} on shard0 and ${finalDocsOnShard1} on shard1`,
);

const metrics = st.config0.getDB("admin").serverStatus({}).shardingStatistics.unshardCollection;

assert.eq(metrics.countStarted, 3, `Expected 3 started but got ${metrics.countStarted}`);
assert.eq(metrics.countSucceeded, 3, `Expected 3 succeeded but got ${metrics.countSucceeded}`);
assert.eq(metrics.countFailed, 0, `Expected 0 failed but got ${metrics.countFailed}`);
assert.eq(metrics.countCanceled, 0, `Expected 0 canceled but got ${metrics.countCanceled}`);

st.stop();
