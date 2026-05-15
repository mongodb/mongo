/**
 * Tests that errorLabels in bulk write command responses are propagated through the bulk API
 * to BulkWriteResult and BulkWriteError.
 *
 * @tags: [
 *   not_allowed_with_signed_security_token,
 *   does_not_support_repeated_reads,
 *   no_selinux,
 * ]
 */

const coll = db[jsTestName()];
coll.drop();

const labelsResult = {
    ok: 1,
    nInserted: 1,
    nUpserted: 0,
    nMatched: 0,
    nModified: 0,
    nRemoved: 0,
    upserted: [],
    writeErrors: [],
    writeConcernErrors: [],
    errorLabels: ["TransientTransactionError"],
};

// BulkWriteResult exposes errorLabels from bulkResult.
const bwr = new BulkWriteResult(labelsResult, null, null);
assert(bwr.hasOwnProperty("errorLabels"), "BulkWriteResult should carry errorLabels: " + tojson(bwr));
assert.eq(["TransientTransactionError"], bwr.errorLabels);

// WriteResult exposes errorLabels from bulkResult.
const wr = new WriteResult(labelsResult, null, null);
assert(wr.hasOwnProperty("errorLabels"), "WriteResult should carry errorLabels: " + tojson(wr));
assert.eq(["TransientTransactionError"], wr.errorLabels);
