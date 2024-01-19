/**
 * Tests bulk write command with bulkWriteMaxRepliesSize against mongos.
 *
 * These tests are incompatible with the transaction overrides since any failure
 * will cause a transaction abortion which will make the overrides infinite loop.
 *
 * @tags: [
 *   # Contains commands that fail which will fail the entire transaction
 *   does_not_support_transactions,
 *   # TODO SERVER-52419 Remove this tag.
 *   featureFlagBulkWriteCommand,
 * ]
 */
import {cursorEntryValidator} from "jstests/libs/bulk_write_utils.js";

const st = new ShardingTest({
    mongos: 1,
    shards: 2,
    rs: {nodes: 1},
    mongosOptions: {setParameter: {featureFlagBulkWriteCommand: true}}
});

const dbName = "test";
const db = st.getDB(dbName);
var coll = db.getCollection("banana");

jsTestLog("Shard the collection.");
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(db.adminCommand({enableSharding: "test"}));
assert.commandWorked(db.adminCommand({shardCollection: "test.banana", key: {a: 1}}));

jsTestLog("Create chunks, then move them.");
assert.commandWorked(db.adminCommand({split: "test.banana", middle: {a: 2}}));
assert.commandWorked(
    db.adminCommand({moveChunk: "test.banana", find: {a: 0}, to: st.shard0.shardName}));
assert.commandWorked(
    db.adminCommand({moveChunk: "test.banana", find: {a: 3}, to: st.shard1.shardName}));

assert.commandWorked(
    st.s.adminCommand({"setParameter": 1, "bulkWriteMaxRepliesSize": NumberInt(20)}));

// Test that replies size limit is hit when bulkWriteMaxRepliesSize is set and ordered = false
let res = st.s.adminCommand({
    bulkWrite: 1,
    ops: [
        {insert: 0, document: {a: -1}},
        {insert: 1, document: {a: 1}},
        {insert: 0, document: {a: 4}}
    ],
    nsInfo: [{ns: "test.banana"}, {ns: "test.orange"}],
    ordered: false
});

jsTestLog("RES");
jsTestLog(res);

assert.commandWorked(res);
assert.eq(res.nErrors, 1);

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 1});
cursorEntryValidator(res.cursor.firstBatch[1],
                     {ok: 0, idx: 1, n: 0, code: ErrorCodes.ExceededMemoryLimit});
assert(!res.cursor.firstBatch[2]);

// Test that replies size limit is hit when bulkWriteMaxRepliesSize is set and ordered = true
res = st.s.adminCommand({
    bulkWrite: 1,
    ops: [
        {insert: 0, document: {a: -1}},
        {insert: 1, document: {a: 1}},
        {insert: 0, document: {a: 4}}
    ],
    nsInfo: [{ns: "test.banana"}, {ns: "test.orange"}],
    ordered: true
});

assert.commandWorked(res);
assert.eq(res.nErrors, 1);

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 1});
cursorEntryValidator(res.cursor.firstBatch[1],
                     {ok: 0, idx: 1, n: 0, code: ErrorCodes.ExceededMemoryLimit});
assert(!res.cursor.firstBatch[2]);

st.stop();
