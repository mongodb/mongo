/**
 * Tests bulk write cursor response for correct responses.
 *
 * @tags: [
 *   # The test runs commands that are not allowed with security token: bulkWrite.
 *   not_allowed_with_signed_security_token,
 *   command_not_supported_in_serverless,
 *   requires_fcv_80
 * ]
 */
import {
    cursorEntryValidator,
    cursorSizeValidator,
    summaryFieldsValidator
} from "jstests/libs/bulk_write_utils.js";

var coll = db.getCollection("coll");
var coll1 = db.getCollection("coll1");
coll.drop();
coll1.drop();

// Test generic delete.
var res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {insert: 0, document: {_id: 1, skey: "MongoDB"}},
        {delete: 0, filter: {_id: 1}},
    ],
    nsInfo: [{ns: "test.coll"}]
});

assert.commandWorked(res);
summaryFieldsValidator(
    res, {nErrors: 0, nInserted: 1, nDeleted: 1, nMatched: 0, nModified: 0, nUpserted: 0});

cursorSizeValidator(res, 2);
cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 1});
cursorEntryValidator(res.cursor.firstBatch[1], {ok: 1, idx: 1, n: 1});

assert(!coll.findOne());

coll.drop();

// Test only deletes one when multi is false (default value).
res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {insert: 0, document: {_id: 0, skey: "MongoDB"}},
        {insert: 0, document: {_id: 1, skey: "MongoDB"}},
        {delete: 0, filter: {skey: "MongoDB"}},
    ],
    nsInfo: [{ns: "test.coll"}]
});

assert.commandWorked(res);
cursorSizeValidator(res, 3);
summaryFieldsValidator(
    res, {nErrors: 0, nInserted: 2, nDeleted: 1, nMatched: 0, nModified: 0, nUpserted: 0});
cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 1});
cursorEntryValidator(res.cursor.firstBatch[1], {ok: 1, idx: 1, n: 1});
cursorEntryValidator(res.cursor.firstBatch[2], {ok: 1, idx: 2, n: 1});
assert.eq(coll.find().itcount(), 1);

coll.drop();

// Test Insert outside of bulkWrite + delete in bulkWrite.
coll.insert({_id: 1, skey: "MongoDB"});

res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {delete: 0, filter: {_id: 1}},
    ],
    nsInfo: [{ns: "test.coll"}]
});

assert.commandWorked(res);
cursorSizeValidator(res, 1);
summaryFieldsValidator(
    res, {nErrors: 0, nInserted: 0, nDeleted: 1, nMatched: 0, nModified: 0, nUpserted: 0});
cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 1});
assert(!coll.findOne());

coll.drop();

// Test delete matches namespace correctly.
coll1.insert({_id: 1, skey: "MongoDB"});

res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {delete: 0, filter: {_id: 1}},
    ],
    nsInfo: [{ns: "test.coll"}]
});

assert.commandWorked(res);
cursorSizeValidator(res, 1);
summaryFieldsValidator(
    res, {nErrors: 0, nInserted: 0, nDeleted: 0, nMatched: 0, nModified: 0, nUpserted: 0});
cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 0});
assert.eq("MongoDB", coll1.findOne().skey);

coll.drop();
coll1.drop();

// Test let matches specific document.
res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {insert: 0, document: {_id: 0, skey: "MongoDB"}},
        {insert: 0, document: {_id: 1, skey: "MongoDB2"}},
        {insert: 0, document: {_id: 2, skey: "MongoDB3"}},
        {delete: 0, filter: {$expr: {$eq: ["$skey", "$$targetKey"]}}},
    ],
    nsInfo: [{ns: "test.coll"}],
    let : {targetKey: "MongoDB"}
});

assert.commandWorked(res);
cursorSizeValidator(res, 4);
summaryFieldsValidator(
    res, {nErrors: 0, nInserted: 3, nDeleted: 1, nMatched: 0, nModified: 0, nUpserted: 0});

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 1});
cursorEntryValidator(res.cursor.firstBatch[1], {ok: 1, idx: 1, n: 1});
cursorEntryValidator(res.cursor.firstBatch[2], {ok: 1, idx: 2, n: 1});
cursorEntryValidator(res.cursor.firstBatch[3], {ok: 1, idx: 3, n: 1});

assert.sameMembers(coll.find().toArray(), [{_id: 1, skey: "MongoDB2"}, {_id: 2, skey: "MongoDB3"}]);

coll.drop();
