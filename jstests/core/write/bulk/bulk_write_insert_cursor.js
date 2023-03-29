/**
 * Tests bulk write cursor response for correct responses.
 *
 * The test runs commands that are not allowed with security token: bulkWrite.
 * @tags: [
 *   assumes_against_mongod_not_mongos,
 *   not_allowed_with_security_token,
 *   # TODO SERVER-72988: Until bulkWrite is compatible with retryable writes.
 *   requires_non_retryable_writes,
 *   # Command is not yet compatible with tenant migration.
 *   tenant_migration_incompatible,
 * ]
 */
(function() {
"use strict";
load("jstests/libs/feature_flag_util.js");

// Skip this test if the BulkWriteCommand feature flag is not enabled.
// TODO SERVER-67711: Remove feature flag check.
if (!FeatureFlagUtil.isPresentAndEnabled(db, "BulkWriteCommand")) {
    jsTestLog('Skipping test because the BulkWriteCommand feature flag is disabled.');
    return;
}

var coll = db.getCollection("coll");
var coll1 = db.getCollection("coll1");
coll.drop();
coll1.drop();

const cursorEntryValidator = function(entry, expectedEntry) {
    assert(entry.ok == expectedEntry.ok);
    assert(entry.idx == expectedEntry.idx);
    assert(entry.n == expectedEntry.n);
    assert(entry.code == expectedEntry.code);
};

// Make sure a properly formed request has successful result.
var res = db.adminCommand(
    {bulkWrite: 1, ops: [{insert: 0, document: {skey: "MongoDB"}}], nsInfo: [{ns: "test.coll"}]});

assert.commandWorked(res);

assert(res.cursor.id == 0);
cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, n: 1, idx: 0});
assert(!res.cursor.firstBatch[1]);

assert.eq(coll.find().itcount(), 1);
assert.eq(coll1.find().itcount(), 0);

coll.drop();

// Test getMore by setting batch size to 1 and running 2 inserts.
// Should end up with 1 insert return per batch.
res = db.adminCommand({
    bulkWrite: 1,
    ops: [{insert: 1, document: {skey: "MongoDB"}}, {insert: 0, document: {skey: "MongoDB"}}],
    nsInfo: [{ns: "test.coll"}, {ns: "test.coll1"}],
    cursor: {batchSize: 1},
});

assert.commandWorked(res);

assert(res.cursor.id != 0);
cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, n: 1, idx: 0});
assert(!res.cursor.firstBatch[1]);

// First batch only had 1 of 2 responses so run a getMore to get the next batch.
var getMoreRes =
    assert.commandWorked(db.adminCommand({getMore: res.cursor.id, collection: "$cmd.bulkWrite"}));

assert(getMoreRes.cursor.id == 0);
cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, n: 1, idx: 0});
assert(!getMoreRes.cursor.nextBatch[1]);

assert.eq(coll.find().itcount(), 1);
assert.eq(coll1.find().itcount(), 1);
coll.drop();
coll1.drop();

// Test internal batch size > 1.
res = db.adminCommand({
    bulkWrite: 1,
    ops: [{insert: 0, document: {skey: "MongoDB"}}, {insert: 0, document: {skey: "MongoDB"}}],
    nsInfo: [{ns: "test.coll"}]
});

assert.commandWorked(res);

assert(res.cursor.id == 0);
cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, n: 1, idx: 0});
cursorEntryValidator(res.cursor.firstBatch[1], {ok: 1, n: 1, idx: 1});
assert(!res.cursor.firstBatch[2]);

assert.eq(coll.find().itcount(), 2);
assert.eq(coll1.find().itcount(), 0);
coll.drop();

// Test that a write can fail part way through a write and the write partially executes.
res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {insert: 0, document: {_id: 1, skey: "MongoDB"}},
        {insert: 0, document: {_id: 1, skey: "MongoDB"}},
        {insert: 1, document: {skey: "MongoDB"}}
    ],
    nsInfo: [{ns: "test.coll"}, {ns: "test.coll1"}]
});

assert.commandWorked(res);

assert(res.cursor.id == 0);
cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, n: 1, idx: 0});
cursorEntryValidator(res.cursor.firstBatch[1], {ok: 0, idx: 1, code: 11000});
// Make sure that error extra info was correctly added
assert.docEq(res.cursor.firstBatch[1].keyPattern, {_id: 1});
assert.docEq(res.cursor.firstBatch[1].keyValue, {_id: 1});
assert(!res.cursor.firstBatch[2]);

assert.eq(coll.find().itcount(), 1);
assert.eq(coll1.find().itcount(), 0);
coll.drop();
coll1.drop();

// Test that we continue processing after an error for ordered:false.
res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {insert: 0, document: {_id: 1, skey: "MongoDB"}},
        {insert: 0, document: {_id: 1, skey: "MongoDB"}},
        {insert: 1, document: {skey: "MongoDB"}}
    ],
    nsInfo: [{ns: "test.coll"}, {ns: "test.coll1"}],
    ordered: false
});

assert.commandWorked(res);

assert(res.cursor.id == 0);
cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, n: 1, idx: 0});
cursorEntryValidator(res.cursor.firstBatch[1], {ok: 0, idx: 1, code: 11000});
cursorEntryValidator(res.cursor.firstBatch[2], {ok: 1, n: 1, idx: 2});
assert(!res.cursor.firstBatch[3]);

assert.eq(coll.find().itcount(), 1);
assert.eq(coll1.find().itcount(), 1);
coll.drop();
coll1.drop();

// Test fixDocumentForInsert works properly by erroring out on >16MB size insert.
var targetSize = (16 * 1024 * 1024) + 1;
var doc = {_id: new ObjectId(), value: ''};

var size = Object.bsonsize(doc);
assert.gte(targetSize, size);

// Set 'value' as a string with enough characters to make the whole document 'targetSize'
// bytes long.
doc.value = new Array(targetSize - size + 1).join('x');
assert.eq(targetSize, Object.bsonsize(doc));

// Testing ordered:false continues on with other ops when fixDocumentForInsert fails.
res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {insert: 0, document: {_id: 1, skey: "MongoDB"}},
        {insert: 0, document: doc},
        {insert: 0, document: {_id: 2, skey: "MongoDB2"}},
    ],
    nsInfo: [{ns: "test.coll"}],
    ordered: false
});

assert.commandWorked(res);

assert(res.cursor.id == 0);
cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, n: 1, idx: 0});

// In most cases we expect this to fail because it tries to insert a document that is too large.
// In some cases we may see the javascript execution interrupted because it takes longer than
// our default time limit, so we allow that possibility.
try {
    cursorEntryValidator(res.cursor.firstBatch[1], {ok: 0, idx: 1, code: ErrorCodes.BadValue});
} catch {
    cursorEntryValidator(res.cursor.firstBatch[1], {ok: 0, idx: 1, code: ErrorCodes.Interrupted});
}
cursorEntryValidator(res.cursor.firstBatch[2], {ok: 1, n: 1, idx: 2});
assert(!res.cursor.firstBatch[3]);

coll.drop();

// Testing ordered:true short circuits.
res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {insert: 0, document: {_id: 1, skey: "MongoDB"}},
        {insert: 0, document: doc},
        {insert: 0, document: {_id: 2, skey: "MongoDB2"}},
    ],
    nsInfo: [{ns: "test.coll"}],
    ordered: true
});

assert.commandWorked(res);

assert(res.cursor.id == 0);
cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, n: 1, idx: 0});

// In most cases we expect this to fail because it tries to insert a document that is too large.
// In some cases we may see the javascript execution interrupted because it takes longer than
// our default time limit, so we allow that possibility.
try {
    cursorEntryValidator(res.cursor.firstBatch[1], {ok: 0, idx: 1, code: ErrorCodes.BadValue});
} catch {
    cursorEntryValidator(res.cursor.firstBatch[1], {ok: 0, idx: 1, code: ErrorCodes.Interrupted});
}
assert(!res.cursor.firstBatch[2]);

coll.drop();
})();
