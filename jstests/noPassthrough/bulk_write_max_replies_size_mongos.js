/**
 * Tests bulk write command with bulkWriteMaxRepliesSize against mongos.
 *
 * These tests are incompatible with the transaction overrides since any failure
 * will cause a transaction abortion which will make the overrides infinite loop.
 *
 * @tags: [
 *   # Contains commands that fail which will fail the entire transaction
 *   does_not_support_transactions,
 *   requires_fcv_80,
 * ]
 */
import {cursorEntryValidator, cursorSizeValidator} from "jstests/libs/bulk_write_utils.js";
import {ShardVersioningUtil} from "jstests/sharding/libs/shard_versioning_util.js";

const st = new ShardingTest({
    mongos: 1,
    shards: 2,
    rs: {nodes: 1},
    mongosOptions: {
        setParameter: {
            featureFlagBulkWriteCommand: true,
            logComponentVerbosity: tojson({command: 4, sharding: 4})
        }
    }
});

const dbName = "test";
const db = st.getDB(dbName);
const coll1 = db.getCollection("banana");
const coll2 = db.getCollection("orange");

const ns1 = "test.banana";
const ns2 = "test.orange";

jsTestLog("Shard the collections.");
assert.commandWorked(coll1.createIndex({a: 1}));
assert.commandWorked(coll2.createIndex({a: 1}));
assert.commandWorked(db.adminCommand({enableSharding: "test"}));
assert.commandWorked(db.adminCommand({shardCollection: ns1, key: {a: 1}}));
assert.commandWorked(db.adminCommand({shardCollection: ns2, key: {a: 1}}));

jsTestLog("Create chunks, then move them.");
assert.commandWorked(db.adminCommand({split: ns1, middle: {a: 2}}));
assert.commandWorked(db.adminCommand({moveChunk: ns1, find: {a: 0}, to: st.shard0.shardName}));
assert.commandWorked(db.adminCommand({moveChunk: ns1, find: {a: 3}, to: st.shard1.shardName}));

// Ensure coll2 initially is on shard0.
assert.commandWorked(db.adminCommand({moveChunk: ns2, find: {a: 0}, to: st.shard0.shardName}));
// Then move its only chunk to shard1 but do not refresh shard1, so that the first write to the
// collection will get a staleness error.
ShardVersioningUtil.moveChunkNotRefreshRecipient(st.s, ns2, null, st.shard1, {a: 0});

assert.commandWorked(
    st.s.adminCommand({"setParameter": 1, "bulkWriteMaxRepliesSize": NumberInt(20)}));

// Test that replies size limit is hit when bulkWriteMaxRepliesSize is set and ordered = false.
// This request should generate two child requests that will be executed in parallel on mongos:
// 1. one to shard0 with the first insert. We should get a single success reply back from the shard.
// 2. one to shard1 with the second and third inserts. We should get a single StaleConfig reply
// back from the shard (even though we are unordered we stop on StaleConfig.)

// Regardless of which reply we get first, after that round of execution we should end up with
// the first insert being marked complete and the next two having been reset to ready. At that
// point, before doing another round of execution, we will check and see we have hit
// bulkWriteMaxRepliesSize and abort execution, marking the second write as failed due to exceeding
// the memory limit.
let res = st.s.adminCommand({
    bulkWrite: 1,
    ops: [
        {insert: 0, document: {a: -1}},
        {insert: 1, document: {a: 1}},
        {insert: 0, document: {a: 4}}
    ],
    nsInfo: [{ns: ns1}, {ns: ns2}],
    ordered: false
});

jsTestLog("RES");
jsTestLog(res);

assert.commandWorked(res);
assert.eq(res.nErrors, 1);
cursorSizeValidator(res, 2);

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 1});
cursorEntryValidator(res.cursor.firstBatch[1],
                     {ok: 0, idx: 1, n: 0, code: ErrorCodes.ExceededMemoryLimit});

// Test that replies size limit is hit when bulkWriteMaxRepliesSize is set and ordered = true.
// This request should generate a child request to shard0 containing the first write, which
// will succeed. After executing it, we will see we have hit the memory limit and stop executing
// the rest of the batch.
res = st.s.adminCommand({
    bulkWrite: 1,
    ops: [
        {insert: 0, document: {a: -1}},
        {insert: 1, document: {a: 1}},
        {insert: 0, document: {a: 4}}
    ],
    nsInfo: [{ns: ns1}, {ns: ns2}],
    ordered: true
});

assert.commandWorked(res);
assert.eq(res.nErrors, 1);
cursorSizeValidator(res, 2);

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 1});
cursorEntryValidator(res.cursor.firstBatch[1],
                     {ok: 0, idx: 1, n: 0, code: ErrorCodes.ExceededMemoryLimit});

st.stop();
