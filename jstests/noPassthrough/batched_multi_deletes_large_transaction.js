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
const ex = assert.throws(() => {
    coll.remove({});
});
jsTestLog('Exception from removing documents: ' + tojson(ex));

// Attempting to log multiple oplog entries for batched deletes should result in a fatal assertion
// with the message: Multi timestamp constraint violated. Transactions setting multiple timestamps
// must set the first timestamp prior to any writes.
assert.soon(function() {
    return rawMongoProgramOutput().search(/Fatal assertion.*4877100/) >= 0;
});

rst.stop(primary.nodeId, /*signal=*/undefined, {allowedExitCode: MongoRunner.EXIT_ABORT});

rst.stopSet();
})();
