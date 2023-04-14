/**
 * Tests bulk write cursor response for correct responses.
 *
 * The test runs commands that are not allowed with security token: bulkWrite.
 * @tags: [
 *   assumes_against_mongod_not_mongos,
 *   not_allowed_with_security_token,
 *   command_not_supported_in_serverless,
 *   # Command is not yet compatible with tenant migration.
 *   tenant_migration_incompatible,
 *   # TODO SERVER-52419 Remove this tag.
 *   featureFlagBulkWriteCommand,
 * ]
 */
(function() {
"use strict";

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
})();
