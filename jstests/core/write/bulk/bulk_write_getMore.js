/**
 * Tests bulk write command in conjunction with using getMore to obtain the rest
 * of the cursor response.
 *
 *
 * @tags: [
 *   # The test runs commands that are not allowed with security token: bulkWrite.
 *   not_allowed_with_security_token,
 *   command_not_supported_in_serverless,
 *   does_not_support_retryable_writes,
 *   requires_non_retryable_writes,
 *   # These tests are incompatible with various overrides due to using getMore.
 *   requires_getmore,
 *   # Contains commands that fail which will fail the entire transaction
 *   does_not_support_transactions,
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
assert.eq(res.cursor.ns, "admin.$cmd.bulkWrite");

// First batch only had 1 of 2 responses so run a getMore to get the next batch.
var getMoreRes =
    assert.commandWorked(db.adminCommand({getMore: res.cursor.id, collection: "$cmd.bulkWrite"}));

assert(getMoreRes.cursor.id == 0);
cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, n: 1, idx: 0});
assert(!getMoreRes.cursor.nextBatch[1]);
assert.eq(res.cursor.ns, "admin.$cmd.bulkWrite");

assert.eq(coll.find().itcount(), 1);
assert.eq(coll1.find().itcount(), 1);
coll.drop();
coll1.drop();

// Want to test ns is properly applied to a cursor that does not need a getMore. This test
// is in this file so it does not run in tenant migration suites since that would change the ns
// name.
res = assert.commandWorked(db.adminCommand(
    {bulkWrite: 1, ops: [{insert: 0, document: {skey: "MongoDB"}}], nsInfo: [{ns: "test.coll"}]}));

assert.commandWorked(res);
assert.eq(res.cursor.ns, "admin.$cmd.bulkWrite");
