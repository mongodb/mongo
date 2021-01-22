/**
 * Test the cursor server status "moreThanOneBatch" and "totalOpened" metric on mongoS.
 *
 * @tags: [requires_fcv_49, requires_sharding]
 */
(function() {
"use strict";

const st = new ShardingTest({shards: 2});
st.stopBalancer();

const db = st.s.getDB("test");
const coll = db.getCollection(jsTestName());

function getNumberOfCursorsOpened() {
    return db.adminCommand({serverStatus: 1}).metrics.mongos.cursor.totalOpened;
}

function getNumberOfCursorsMoreThanOneBatch() {
    return db.adminCommand({serverStatus: 1}).metrics.mongos.cursor.moreThanOneBatch;
}

coll.drop();
assert.commandWorked(db.adminCommand({enableSharding: db.getName()}));
st.ensurePrimaryShard(db.getName(), st.shard0.shardName);
db.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}});
assert.commandWorked(db.adminCommand({split: coll.getFullName(), middle: {_id: 0}}));

jsTestLog("Inserting documents into the collection: " + jsTestName());
for (let i = -4; i < 4; i++) {
    assert.commandWorked(coll.insert({_id: i, a: 4 * i, b: "hello"}));
}

const initialNumCursorsOpened = getNumberOfCursorsOpened();
const initialNumCursorsMoreThanOneBatch = getNumberOfCursorsMoreThanOneBatch();
jsTestLog("Cursors opened initially: " + initialNumCursorsOpened);
jsTestLog("Cursors with more than one batch initially: " + initialNumCursorsMoreThanOneBatch);

jsTestLog("Running find.");
let cmdRes = assert.commandWorked(db.runCommand({find: coll.getName(), batchSize: 2}));
let cursorId = cmdRes.cursor.id;
assert.eq(getNumberOfCursorsOpened() - initialNumCursorsOpened, 1, cmdRes);
assert.eq(getNumberOfCursorsMoreThanOneBatch() - initialNumCursorsMoreThanOneBatch, 0, cmdRes);

jsTestLog("Killing cursor with cursorId: " + cursorId);
assert.commandWorked(db.runCommand({killCursors: coll.getName(), cursors: [cursorId]}));
assert.eq(getNumberOfCursorsOpened() - initialNumCursorsOpened, 1, cmdRes);
assert.eq(getNumberOfCursorsMoreThanOneBatch() - initialNumCursorsMoreThanOneBatch, 0, cmdRes);

jsTestLog("Running second find, this time will run getMore.");
cmdRes = assert.commandWorked(db.runCommand({find: coll.getName(), batchSize: 2}));
cursorId = cmdRes.cursor.id;
assert.eq(getNumberOfCursorsOpened() - initialNumCursorsOpened, 2, cmdRes);
assert.eq(getNumberOfCursorsMoreThanOneBatch() - initialNumCursorsMoreThanOneBatch, 0, cmdRes);

jsTestLog("Running getMore for cursorId: " + cursorId);
cmdRes = assert.commandWorked(
    db.runCommand({getMore: cursorId, collection: coll.getName(), batchSize: 2}));
// Expect the number of cursors with more than one batch to not have changed.
assert.eq(getNumberOfCursorsOpened() - initialNumCursorsOpened, 2, cmdRes);
assert.eq(getNumberOfCursorsMoreThanOneBatch() - initialNumCursorsMoreThanOneBatch, 0, cmdRes);

jsTestLog("Killing cursor with cursorId: " + cursorId);
assert.commandWorked(db.runCommand({killCursors: coll.getName(), cursors: [cursorId]}));
assert.eq(getNumberOfCursorsOpened() - initialNumCursorsOpened, 2, cmdRes);
assert.eq(getNumberOfCursorsMoreThanOneBatch() - initialNumCursorsMoreThanOneBatch, 1, cmdRes);

jsTestLog("Running aggregate command.");
cmdRes = assert.commandWorked(
    db.runCommand({aggregate: coll.getName(), pipeline: [], cursor: {batchSize: 2}}));
cursorId = cmdRes.cursor.id;
assert.eq(getNumberOfCursorsOpened() - initialNumCursorsOpened, 3, cmdRes);
assert.eq(getNumberOfCursorsMoreThanOneBatch() - initialNumCursorsMoreThanOneBatch, 1, cmdRes);

jsTestLog("Running getMore on aggregate cursor: " + cursorId);
cmdRes = assert.commandWorked(
    db.runCommand({getMore: cursorId, collection: coll.getName(), batchSize: 2}));
assert.eq(getNumberOfCursorsOpened() - initialNumCursorsOpened, 3, cmdRes);
assert.eq(getNumberOfCursorsMoreThanOneBatch() - initialNumCursorsMoreThanOneBatch, 1, cmdRes);

// Use a batchSize that's greater than the number of documents and therefore exhaust the cursor.
jsTestLog("Exhausting cursor with cursorId: " + cursorId);
cmdRes = assert.commandWorked(
    db.runCommand({getMore: cursorId, collection: coll.getName(), batchSize: 20}));
assert.eq(getNumberOfCursorsOpened() - initialNumCursorsOpened, 3, cmdRes);
assert.eq(getNumberOfCursorsMoreThanOneBatch() - initialNumCursorsMoreThanOneBatch, 2, cmdRes);

st.stop();
})();
