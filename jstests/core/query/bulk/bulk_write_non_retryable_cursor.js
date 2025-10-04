/**
 * Tests bulk write cursor response for correct responses.
 *
 * This file contains tests that are not compatible with retryable writes for various reasons.
 *
 * @tags: [
 *   does_not_support_retryable_writes,
 *   requires_non_retryable_writes,
 *   #  The test runs commands that are not allowed with security token: bulkWrite.
 *   not_allowed_with_signed_security_token,
 *   command_not_supported_in_serverless,
 *   requires_fcv_80
 * ]
 */
import {cursorEntryValidator, cursorSizeValidator, summaryFieldsValidator} from "jstests/libs/bulk_write_utils.js";

const coll = db[jsTestName()];
const collName = coll.getFullName();
const coll1 = db[jsTestName() + "1"];
coll.drop();
coll1.drop();

// Multi:true is not supported for retryable writes.

// Test updates multiple when multi is true.
let res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {insert: 0, document: {_id: 0, skey: "MongoDB"}},
        {insert: 0, document: {_id: 1, skey: "MongoDB"}},
        {update: 0, filter: {skey: "MongoDB"}, updateMods: {$set: {skey: "MongoDB2"}}, multi: true},
    ],
    nsInfo: [{ns: collName}],
});

assert.commandWorked(res);
cursorSizeValidator(res, 3);
summaryFieldsValidator(res, {nErrors: 0, nInserted: 2, nDeleted: 0, nMatched: 2, nModified: 2, nUpserted: 0});

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 1});
cursorEntryValidator(res.cursor.firstBatch[1], {ok: 1, idx: 1, n: 1});
cursorEntryValidator(res.cursor.firstBatch[2], {ok: 1, idx: 2, n: 2, nModified: 2});

assert.sameMembers(coll.find().toArray(), [
    {_id: 0, skey: "MongoDB2"},
    {_id: 1, skey: "MongoDB2"},
]);

assert(coll.drop());

// Test deletes multiple when multi is true.
res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {insert: 0, document: {_id: 0, skey: "MongoDB"}},
        {insert: 0, document: {_id: 1, skey: "MongoDB"}},
        {delete: 0, filter: {skey: "MongoDB"}, multi: true},
    ],
    nsInfo: [{ns: collName}],
});

assert.commandWorked(res);
cursorSizeValidator(res, 3);
summaryFieldsValidator(res, {nErrors: 0, nInserted: 2, nDeleted: 2, nMatched: 0, nModified: 0, nUpserted: 0});

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 1});
cursorEntryValidator(res.cursor.firstBatch[1], {ok: 1, idx: 1, n: 1});
cursorEntryValidator(res.cursor.firstBatch[2], {ok: 1, idx: 2, n: 2});
assert(!coll.findOne());

assert(coll.drop());

// Test let for multiple updates and a delete, with constants shadowing in one update.
res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {insert: 0, document: {_id: 0, skey: "MongoDB"}},
        {insert: 0, document: {_id: 1, skey: "MongoDB2"}},
        {
            update: 0,
            filter: {$expr: {$eq: ["$skey", "$$targetKey1"]}},
            updateMods: [{$set: {skey: "$$replacedKey1"}}],
            constants: {replacedKey1: "MongoDB4"},
        },
        {
            update: 0,
            filter: {$expr: {$eq: ["$skey", "$$targetKey2"]}},
            updateMods: [{$set: {skey: "MongoDB"}}],
        },
        {delete: 0, filter: {$expr: {$eq: ["$skey", "$$replacedKey2"]}}},
    ],
    nsInfo: [{ns: collName}],
    let: {
        targetKey1: "MongoDB",
        targetKey2: "MongoDB2",
        replacedKey1: "MongoDB",
        replacedKey2: "MongoDB4",
    },
});

assert.commandWorked(res);
cursorSizeValidator(res, 5);
summaryFieldsValidator(res, {nErrors: 0, nInserted: 2, nDeleted: 1, nMatched: 2, nModified: 2, nUpserted: 0});

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 1});
cursorEntryValidator(res.cursor.firstBatch[1], {ok: 1, idx: 1, n: 1});
cursorEntryValidator(res.cursor.firstBatch[2], {ok: 1, idx: 2, n: 1, nModified: 1});
cursorEntryValidator(res.cursor.firstBatch[3], {ok: 1, idx: 3, n: 1, nModified: 1});
cursorEntryValidator(res.cursor.firstBatch[4], {ok: 1, idx: 4, n: 1});

assert.sameMembers(coll.find().toArray(), [{_id: 1, skey: "MongoDB"}]);

assert(coll.drop());
