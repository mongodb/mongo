/**
 * Tests bulk write cursor response for correct responses.
 *
 * This file contains tests that are not compatible with retryable writes for various reasons.
 *
 * @tags: [
 *   does_not_support_retryable_writes,
 *   requires_non_retryable_writes,
 *   #  The test runs commands that are not allowed with security token: bulkWrite.
 *   not_allowed_with_security_token,
 *   command_not_supported_in_serverless,
 *   # TODO SERVER-52419 Remove this tag.
 *   featureFlagBulkWriteCommand,
 *   # TODO SERVER-79506 Remove this tag.
 *   assumes_unsharded_collection,
 * ]
 */
import {cursorEntryValidator} from "jstests/libs/bulk_write_utils.js";

var coll = db.getCollection("coll");
var coll1 = db.getCollection("coll1");
coll.drop();
coll1.drop();

// Multi:true is not supported for retryable writes.

// Test updates multiple when multi is true.
var res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {insert: 0, document: {_id: 0, skey: "MongoDB"}},
        {insert: 0, document: {_id: 1, skey: "MongoDB"}},
        {update: 0, filter: {skey: "MongoDB"}, updateMods: {$set: {skey: "MongoDB2"}}, multi: true},
    ],
    nsInfo: [{ns: "test.coll"}]
});

assert.commandWorked(res);
assert.eq(res.numErrors, 0);

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 1});
cursorEntryValidator(res.cursor.firstBatch[1], {ok: 1, idx: 1, n: 1});
cursorEntryValidator(res.cursor.firstBatch[2], {ok: 1, idx: 2, n: 2, nModified: 2});
assert(!res.cursor.firstBatch[2].value);
assert(!res.cursor.firstBatch[3]);
assert.sameMembers(coll.find().toArray(), [{_id: 0, skey: "MongoDB2"}, {_id: 1, skey: "MongoDB2"}]);

coll.drop();

// Test deletes multiple when multi is true.
res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {insert: 0, document: {_id: 0, skey: "MongoDB"}},
        {insert: 0, document: {_id: 1, skey: "MongoDB"}},
        {delete: 0, filter: {skey: "MongoDB"}, multi: true},
    ],
    nsInfo: [{ns: "test.coll"}]
});

assert.commandWorked(res);
assert.eq(res.numErrors, 0);

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 1});
cursorEntryValidator(res.cursor.firstBatch[1], {ok: 1, idx: 1, n: 1});
cursorEntryValidator(res.cursor.firstBatch[2], {ok: 1, idx: 2, n: 2});
assert(!res.cursor.firstBatch[3]);
assert(!coll.findOne());

coll.drop();

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
            constants: {replacedKey1: "MongoDB4"}
        },
        {
            update: 0,
            filter: {$expr: {$eq: ["$skey", "$$targetKey2"]}},
            updateMods: [{$set: {skey: "MongoDB"}}]
        },
        {delete: 0, filter: {$expr: {$eq: ["$skey", "$$replacedKey2"]}}}
    ],
    nsInfo: [{ns: "test.coll"}],
    let : {
        targetKey1: "MongoDB",
        targetKey2: "MongoDB2",
        replacedKey1: "MongoDB",
        replacedKey2: "MongoDB4"
    }
});

assert.commandWorked(res);
assert.eq(res.numErrors, 0);

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 1});
cursorEntryValidator(res.cursor.firstBatch[1], {ok: 1, idx: 1, n: 1});
cursorEntryValidator(res.cursor.firstBatch[2], {ok: 1, idx: 2, n: 1, nModified: 1});
cursorEntryValidator(res.cursor.firstBatch[3], {ok: 1, idx: 3, n: 1, nModified: 1});
cursorEntryValidator(res.cursor.firstBatch[4], {ok: 1, idx: 4, n: 1});
assert(!res.cursor.firstBatch[5]);

assert.sameMembers(coll.find().toArray(), [{_id: 1, skey: "MongoDB"}]);

coll.drop();
