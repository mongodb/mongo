/**
 * Tests bulk write command in conjunction with using getMore to obtain the rest
 * of the cursor response. This test explicitly tests retrayble writes so cannot be used with the
 * transaction override.
 *
 *
 * @tags: [
 *   # The test runs commands that are not allowed with security token: bulkWrite.
 *   not_allowed_with_signed_security_token,
 *   command_not_supported_in_serverless,
 *   does_not_support_retryable_writes,
 *   requires_non_retryable_writes,
 *   # These tests are incompatible with various overrides due to using getMore.
 *   requires_getmore,
 *   # network_error_and_txn_override does not append txnNumber to getMores.
 *   does_not_support_transactions,
 *   requires_fcv_80
 * ]
 */
import {
    cursorEntryValidator,
    cursorSizeValidator,
    summaryFieldsValidator
} from "jstests/libs/bulk_write_utils.js";

// The retryable write override does not append txnNumber to getMore since it is not a retryable
// command so we need to test it manually.

function runTest(retryableWrite) {
    var coll = db.getCollection("coll");
    var coll1 = db.getCollection("coll1");
    coll.drop();
    coll1.drop();

    // Test getMore by setting batch size to 1 and running 2 inserts.
    // Should end up with 1 insert return per batch.
    var cmdObj = {
        bulkWrite: 1,
        ops: [{insert: 1, document: {skey: "MongoDB"}}, {insert: 0, document: {skey: "MongoDB"}}],
        nsInfo: [{ns: "test.coll"}, {ns: "test.coll1"}],
        cursor: {batchSize: 1},
    };

    if (retryableWrite) {
        cmdObj.txnNumber = NumberLong(0);
    }

    var res = db.adminCommand(cmdObj);

    assert.commandWorked(res);
    cursorSizeValidator(res, 1);
    summaryFieldsValidator(
        res, {nErrors: 0, nInserted: 2, nDeleted: 0, nMatched: 0, nModified: 0, nUpserted: 0});

    assert(res.cursor.id != 0,
           "Unexpectedly found cursor ID 0 in bulkWrite command response: " + tojson(res));
    cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, n: 1, idx: 0});
    assert.eq(res.cursor.ns,
              "admin.$cmd.bulkWrite",
              "Found unexpected ns in bulkWrite command response: " + tojson(res));

    // First batch only had 1 of 2 responses so run a getMore to get the next batch.
    var getMoreRes = assert.commandWorked(
        db.adminCommand({getMore: res.cursor.id, collection: "$cmd.bulkWrite"}));
    assert(!getMoreRes.cursor.nextBatch[1],
           "Unexpectedly found cursor entry at index 1 in getMore command response: " +
               tojson(getMoreRes));
    assert(
        getMoreRes.cursor.id == 0,
        "Unexpectedly found non-zero cursor ID in getMore command response: " + tojson(getMoreRes));
    cursorEntryValidator(getMoreRes.cursor.nextBatch[0], {ok: 1, n: 1, idx: 1});
    assert.eq(getMoreRes.cursor.ns,
              "admin.$cmd.bulkWrite",
              "Found unexpected ns in getMore command response: " + tojson(res));

    assert.eq(coll.find().itcount(), 1);
    assert.eq(coll1.find().itcount(), 1);
    coll.drop();
    coll1.drop();

    // TODO SERVER-97170 check if the following is still necessary
    // Want to test ns is properly applied to a cursor that does not need a getMore. This test
    // is in this file so it does not run in suites since that would change the ns
    // name.
    res = assert.commandWorked(db.adminCommand({
        bulkWrite: 1,
        ops: [{insert: 0, document: {skey: "MongoDB"}}],
        nsInfo: [{ns: "test.coll"}]
    }));

    assert.commandWorked(res);
    assert.eq(res.cursor.ns,
              "admin.$cmd.bulkWrite",
              "Found unexpected ns in bulkWrite command response: " + tojson(res));

    coll.drop();

    // Make sure that a getMore that occurs naturally (16MB size limit) also works.
    coll.insert({_id: 1});

    let bulkOps = [];

    for (let i = 0; i < 100000; i++) {
        bulkOps.push({insert: 0, document: {_id: 1}});
    }

    cmdObj = {bulkWrite: 1, ops: bulkOps, nsInfo: [{ns: "test.coll"}], ordered: false};

    if (retryableWrite) {
        cmdObj.txnNumber = NumberLong(0);
    }

    // Make sure a properly formed request has successful result.
    var res = assert.commandWorked(db.adminCommand(cmdObj));

    // All these writes are against the same unsharded collection and should be sent in one batch to
    // a single mongos. If this causes a getMore to be needed on the result from mongos that means
    // we would have needed to run a getMore against mongod.
    assert.neq(res.cursor.id, 0);
}

runTest(false /* retryableWrite */);
runTest(true, /* retryableWrite */);
