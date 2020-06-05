/**
 * Tests readConcern level snapshot outside of transactions, targeting one shard out of two. The
 * snapshot timestamp selection algorithm is different for single-shard reads from a sharded
 * collection (SERVER-47952), make sure mongos returns the correct atClusterTime with cursor
 * replies.
 *
 * @tags: [
 *   requires_fcv_46,
 *   requires_majority_read_concern,
 *   requires_find_command
 * ]
 */

(function() {
"use strict";

load("jstests/libs/global_snapshot_reads_util.js");
load("jstests/sharding/libs/sharded_transactions_helpers.js");

const nodeOptions = {
    // Set a large snapshot window of 10 minutes for the test.
    setParameter: {minSnapshotHistoryWindowInSeconds: 600}
};

const dbName = "test";
const collName = "collection";

const st =
    new ShardingTest({shards: 2, other: {configOptions: nodeOptions, rsOptions: nodeOptions}});

jsTestLog("initiated");
const mongos = st.s0;

assert.commandWorked(mongos.adminCommand({enableSharding: dbName}));
assert.commandWorked(mongos.adminCommand({shardCollection: "test.collection", key: {_id: 1}}));

const ns = dbName + "." + collName;
assert.commandWorked(st.splitAt(ns, {_id: 5}));

assert.commandWorked(mongos.adminCommand({moveChunk: ns, find: {_id: 0}, to: st.shard0.shardName}));
assert.commandWorked(mongos.adminCommand({moveChunk: ns, find: {_id: 9}, to: st.shard1.shardName}));

const db = mongos.getDB(dbName);
const docs = [...Array(10).keys()].map((i) => ({"_id": i}));
assert.commandWorked(
    db.runCommand({insert: collName, documents: docs, writeConcern: {w: "majority"}}));

jsTestLog("Advance mongos's clusterTime by writing to shard 1");

const insertReply = assert.commandWorked(db.runCommand({insert: collName, documents: [{_id: 10}]}));

jsTestLog(`Wrote to shard 1 at timestamp ${insertReply.operationTime}`);

jsTestLog("Read from shard 0");

let reply0 = assert.commandWorked(db.runCommand(
    {find: collName, filter: {_id: {$lt: 4}}, batchSize: 1, readConcern: {level: "snapshot"}}));

jsTestLog(`find reply: ${tojson(reply0)}`);

let cursorId = reply0.cursor.id;
assert.neq(cursorId, 0);
assert(reply0.cursor.hasOwnProperty("atClusterTime"));
assert.lt(reply0.cursor.atClusterTime, insertReply.operationTime);

jsTestLog("Write to shard 0");

const updateReply = assert.commandWorked(db.runCommand({
    update: collName,
    updates: [{q: {_id: {$lt: 4}}, u: {$set: {x: 1}}}],
    writeConcern: {w: "majority"}
}));

jsTestLog(`Wrote to shard 0 at timestamp ${updateReply.operationTime}`);

let reply1 =
    assert.commandWorked(db.runCommand({getMore: cursorId, collection: collName, batchSize: 1}));

jsTestLog(`getMore reply: ${tojson(reply1)}`);

assert(reply1.cursor.hasOwnProperty("atClusterTime"));
assert.eq(reply0.cursor.atClusterTime, reply1.cursor.atClusterTime);

// We have read the version of the document prior to the update.
assert(!reply1.cursor.nextBatch[0].hasOwnProperty("x"));

st.stop();
})();
