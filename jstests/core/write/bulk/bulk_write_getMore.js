/**
 * Tests bulk write command in conjunction with using getMore to obtain the rest
 * of the cursor response.
 *
 * These tests are incompatible with various overrides due to using getMore.
 *
 * The test runs commands that are not allowed with security token: bulkWrite.
 * @tags: [
 *   assumes_against_mongod_not_mongos,
 *   not_allowed_with_security_token,
 *   command_not_supported_in_serverless,
 *   does_not_support_retryable_writes,
 *   requires_non_retryable_writes,
 *   requires_getmore,
 *   # Contains commands that fail which will fail the entire transaction
 *   does_not_support_transactions,
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
    assert(entry.nModified == expectedEntry.nModified);
    assert(entry.code == expectedEntry.code);
};

// The retryable write override does not append txnNumber to getMore since it is not a retryable
// command.

// Test getMore by setting batch size to 1 and running 2 inserts.
// Should end up with 1 insert return per batch.
var res = db.adminCommand({
    bulkWrite: 1,
    ops: [{insert: 1, document: {skey: "MongoDB"}}, {insert: 0, document: {skey: "MongoDB"}}],
    nsInfo: [{ns: "test.coll"}, {ns: "test.coll1"}],
    cursor: {batchSize: 1},
});

assert.commandWorked(res);
assert.eq(res.numErrors, 0);

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
})();
