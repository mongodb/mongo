/**
 * Tests that a multi-oplog batched multi delete operation can be rolled back.
 * @tags: [
 *   requires_fcv_70,
 *   requires_replication,
 * ]
 */
(function() {
'use strict';

load('jstests/replsets/libs/rollback_test.js');
load("jstests/libs/feature_flag_util.js");  // for FeatureFlagUtil.isEnabled

// Operations that will be present on both nodes, before the common point.
const dbName = 'test';
const collName = 'test.t';
const collNameShort = 't';
const docIds = [0, 1, 2, 3];
let CommonOps = (node) => {
    const coll = node.getCollection(collName);
    assert.commandWorked(coll.insert(docIds.map((x) => {
        return {_id: x, x: x};
    })));
};

// Operations that will be performed on the rollback node past the common point.
let RollbackOps = (node, rst) => {
    jsTestLog("NODE PORT: " + node.port);
    const coll = node.getCollection(collName);
    const result = assert.commandWorked(coll.remove({}));
    jsTestLog('delete result: ' + tojson(result));
    assert.eq(result.nRemoved, docIds.length);
    assert.eq(coll.countDocuments({}), 0);

    // Check oplog entries generated for the large batched write.
    // Query returns any oplog entries where the applyOps array contains an operation with a
    // namespace matching the test collection namespace. Oplog entries will be returned in reverse
    // timestamp order (most recent first).
    const ops = rst.findOplog(node, {
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
};

// Set up Rollback Test.
const nodeOptions = {
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
    },
};
const rollbackTest = new RollbackTest(jsTestName(), /*replSet=*/ undefined, nodeOptions);

if (!FeatureFlagUtil.isEnabled(rollbackTest.getPrimary(),
                               "InternalWritesAreReplicatedTransactionally")) {
    jsTestLog('Skipping test because required feature flag is not enabled.');
    rollbackTest.stop();
    return;
}

CommonOps(rollbackTest.getPrimary());

const rollbackNode = rollbackTest.transitionToRollbackOperations();
const rst = rollbackTest.getTestFixture();
RollbackOps(rollbackNode, rst);

// Wait for rollback to finish.
rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
rollbackTest.transitionToSyncSourceOperationsDuringRollback();
rollbackTest.transitionToSteadyStateOperations();

// Check collection count.
const primary = rollbackTest.getPrimary();
const coll = primary.getCollection(collName);
assert.eq(docIds.length, coll.countDocuments({}));

rollbackTest.stop();
})();
