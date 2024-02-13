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

// Make sure a properly formed request has successful result.
var res = db.adminCommand(
    {bulkWrite: 1, ops: [{insert: 0, document: {skey: "MongoDB"}}], nsInfo: [{ns: "test.coll"}]});

assert.commandWorked(res);
cursorSizeValidator(res, 1);
summaryFieldsValidator(
    res, {nErrors: 0, nInserted: 1, nDeleted: 0, nMatched: 0, nModified: 0, nUpserted: 0});

assert(res.cursor.id == 0,
       "Unexpectedly found non-zero cursor ID in bulkWrite command response: " + tojson(res));
cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, n: 1, idx: 0});

assert.eq(coll.find().itcount(), 1);
assert.eq(coll1.find().itcount(), 0);

coll.drop();

// Test internal batch size > 1.
res = db.adminCommand({
    bulkWrite: 1,
    ops: [{insert: 0, document: {skey: "MongoDB"}}, {insert: 0, document: {skey: "MongoDB"}}],
    nsInfo: [{ns: "test.coll"}]
});

assert.commandWorked(res);
cursorSizeValidator(res, 2);
summaryFieldsValidator(
    res, {nErrors: 0, nInserted: 2, nDeleted: 0, nMatched: 0, nModified: 0, nUpserted: 0});

assert(res.cursor.id == 0,
       "Unexpectedly found non-zero cursor ID in bulkWrite command response: " + tojson(res));
cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, n: 1, idx: 0});
cursorEntryValidator(res.cursor.firstBatch[1], {ok: 1, n: 1, idx: 1});

assert.eq(coll.find().itcount(), 2);
assert.eq(coll1.find().itcount(), 0);
coll.drop();

// Test errorsOnly with no failues.
res = db.adminCommand({
    bulkWrite: 1,
    ops: [{insert: 0, document: {_id: 1}}, {insert: 0, document: {_id: 2}}],
    nsInfo: [{ns: "test.coll"}],
    errorsOnly: true
});

assert.commandWorked(res, "bulkWrite command response: " + tojson(res));
cursorSizeValidator(res, 0);
summaryFieldsValidator(
    res, {nErrors: 0, nInserted: 2, nDeleted: 0, nMatched: 0, nModified: 0, nUpserted: 0});

assert(res.cursor.id == 0, "bulkWrite command response: " + tojson(res));
assert(!res.cursor.firstBatch[0], "bulkWrite command response: " + tojson(res));
assert.eq(res.cursor.ns, "admin.$cmd.bulkWrite", "bulkWrite command response: " + tojson(res));

coll.drop();
