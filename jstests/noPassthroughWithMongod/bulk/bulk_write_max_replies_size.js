/**
 * Tests bulk write command with bulkWriteMaxRepliesSize.
 *
 * These tests are incompatible with the transaction overrides since any failure
 * will cause a transaction abortion which will make the overrides infinite loop.
 *
 * @tags: [
 *   # Contains commands that fail which will fail the entire transaction
 *   does_not_support_transactions,
 *   requires_fcv_80
 * ]
 */
import {cursorEntryValidator} from "jstests/libs/bulk_write_utils.js";

var coll = db.getCollection("coll");
coll.drop();

assert.commandWorked(
    db.adminCommand({"setParameter": 1, "bulkWriteMaxRepliesSize": NumberInt(20)}));

// Test that replies size limit is hit when bulkWriteMaxRepliesSize is set and ordered = false
let res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {insert: 0, document: {_id: 0, skey: "MongoDB"}},
        {update: 0, filter: {_id: 0}, updateMods: {$set: {a: 2}}},
        {insert: 0, document: {_id: 1, skey: "MongoDB"}}
    ],
    nsInfo: [{ns: "test.coll"}],
    ordered: false
});

assert.commandWorked(res);
assert.eq(res.nErrors, 1);

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 1});
cursorEntryValidator(res.cursor.firstBatch[1],
                     {ok: 0, idx: 1, n: 0, code: ErrorCodes.ExceededMemoryLimit});
assert(!res.cursor.firstBatch[2]);

// Test that replies size limit is hit when bulkWriteMaxRepliesSize is set and ordered = true
res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {insert: 0, document: {_id: 1, skey: "MongoDB"}},
        {update: 0, filter: {_id: 1}, updateMods: {$set: {a: 2}}},
        {insert: 0, document: {_id: 2, skey: "MongoDB"}}
    ],
    nsInfo: [{ns: "test.coll"}],
    ordered: true
});

assert.commandWorked(res);
assert.eq(res.nErrors, 1);

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 1});
cursorEntryValidator(res.cursor.firstBatch[1],
                     {ok: 0, idx: 1, n: 0, code: ErrorCodes.ExceededMemoryLimit});
assert(!res.cursor.firstBatch[2]);

coll.drop();

assert.commandWorked(
    db.adminCommand({"setParameter": 1, "bulkWriteMaxRepliesSize": NumberInt(30 * 1024 * 1024)}));
