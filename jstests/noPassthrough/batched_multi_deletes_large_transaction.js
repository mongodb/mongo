/**
 * Tests that batched multi-deletes can span multiple oplog entries.
 *
 * This is done by constraining the number of write operations contained in
 * each replicated applyOps oplog entry to show how "large" batched writes are
 * handled by the primary.
 *
 * @tags: [
 *     requires_fcv_62,
 *     requires_replication,
 * ]
 */
(function() {
'use strict';

load("jstests/libs/feature_flag_util.js");  // for FeatureFlagUtil.isEnabled

const rst = new ReplSetTest({
    nodes: [
        {
            setParameter: {
                // On a slow machine, the default of 5 ms for 'batchedDeletesTargetBatchTimeMS'
                // may cause the batched delete stage to break up the delete operation into
                // smaller batches that not allow us to test the effect of
                // 'maxNumberOfBatchedOperationsInSingleOplogEntry' on
                // OpObserver::onBatchedWriteCommit().
                // Setting 'batchedDeletesTargetBatchTimeMS' to zero (for unlimited) ensures that
                // the delete stage will alway put all the requested deletes in a single batch.
                batchedDeletesTargetBatchTimeMS: 0,
                maxNumberOfBatchedOperationsInSingleOplogEntry: 2,
            }
        },
        {rsConfig: {votes: 0, priority: 0}},
    ]
});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();
const db = primary.getDB('test');
const coll = db.t;

const docIds = [0, 1, 2, 3];
assert.commandWorked(coll.insert(docIds.map((x) => {
    return {_id: x, x: x};
})));

// Set up server to split deletes over multiple oplog entries
// such that each oplog entry contains two delete operations.
if (!FeatureFlagUtil.isEnabled(db, "InternalWritesAreReplicatedTransactionally")) {
    // Confirm legacy server behavior where mutiple oplog entries are not allowed
    // for batched writes.
    const result =
        assert.commandFailedWithCode(coll.remove({}),
                                     ErrorCodes.TransactionTooLarge,
                                     'batched writes must generate a single applyOps entry');
    jsTestLog('delete result: ' + tojson(result));
    assert.eq(result.nRemoved, 0);
    assert.eq(coll.countDocuments({}), docIds.length);

    // Stop test and return early. The rest of the test will test the new multiple oplog entry
    // behavior.
    rst.stopSet();
    return;
}

// This document removal request will be replicated over two applyOps oplog entries,
// each containing two delete operations.
const result = assert.commandWorked(coll.remove({}));
jsTestLog('delete result: ' + tojson(result));
assert.eq(result.nRemoved, docIds.length);
assert.eq(coll.countDocuments({}), 0);

// Check oplog entries generated for the large batched write.
// Query returns any oplog entries where the applyOps array contains an operation with a namespace
// matching the test collection namespace.
// Oplog entries will be returned in reverse timestamp order (most recent first).
const ops = rst.findOplog(primary, {
                   op: 'c',
                   ns: 'admin.$cmd',
                   'o.applyOps': {$elemMatch: {op: 'd', ns: coll.getFullName()}}
               })
                .toArray();
jsTestLog('applyOps oplog entries: ' + tojson(ops));
assert.eq(2, ops.length, 'Should have two applyOps oplog entries');
const deletedDocIds = ops.map((entry) => entry.o.applyOps.map((op) => op.o._id)).flat();
jsTestLog('deleted doc _ids: ' + tojson(deletedDocIds));
assert.sameMembers(deletedDocIds, docIds);
assert.eq(ops[0].o.count,
          docIds.length,
          'last oplog entry should contain total count of operations in chain: ' + tojson(ops[0]));
assert(ops[1].o.partialTxn,
       'non-terminal oplog entry should have partialTxn field set to true: ' + tojson(ops[1]));
assert(ops[0].hasOwnProperty('prevOpTime'));
assert(ops[1].hasOwnProperty('prevOpTime'));
assert.eq(ops[0].prevOpTime.ts, ops[1].ts);
assert.eq(ops[1].prevOpTime.ts, Timestamp());

// Secondary oplog application will reject the first applyOps entry in the oplog chain because it
// is expecting a multi-document transaction with the 'lsid' and 'txnNumber' fields.
// TODO(SERVER-70572): Remove this check when oplog application supports chained applyOps entries
// for batch writes.
assert.soon(function() {
    return rawMongoProgramOutput().search(/Invariant failure.*op\.getSessionId\(\)/) >= 0;
});
const secondary = rst.getSecondary();
rst.stop(secondary, /*signal=*/undefined, {allowedExitCode: MongoRunner.EXIT_ABORT});

// TODO(SERVER-70572): Secondary oplog application cannot apply chain of applyOps oplog entries
// for batched writes, so the collections in a replica set will be inconsistent.
// Therefore, we skip checking collection/db hashes across a replica set until this is resolved.
rst.stopSet(/*signal=*/undefined, /*forRestart=*/false, {skipCheckDBHashes: true});
})();
