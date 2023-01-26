/**
 * Tests that multi-deletes in a mixed version cluster replicate as individual delete operations.
 *
 * Batched multi-deletes were introduced in 6.1 so a replica set running in 6.0 FCV will not be
 * able to take advantage of this feature.
 *
 * @tags: [
 *     requires_replication,
 * ]
 */
(function() {
'use strict';

function runTest(primaryBinVersion, secondaryBinVersion) {
    const testLogPrefix =
        'primary-' + primaryBinVersion + '-secondary-' + secondaryBinVersion + ': ';
    jsTestLog(testLogPrefix + 'Starting test case.');
    const rst = new ReplSetTest({
        nodes: [
            {
                binVersion: primaryBinVersion,
            },
            {
                binVersion: secondaryBinVersion,
                rsConfig: {votes: 0, priority: 0},
            },
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

    assert.commandWorked(coll.remove({}));
    // Check oplog entries generated for the multi-delete operation.
    // Oplog entries will be returned in reverse timestamp order (most recent first).
    const ops = rst.findOplog(primary, {op: 'd', ns: coll.getFullName()}).toArray();
    jsTestLog(testLogPrefix + 'applyOps oplog entries: ' + tojson(ops));
    assert.eq(ops.length,
              docIds.length,
              'number oplog entries should match documents inserted initially');
    const deletedDocIds = ops.map((entry) => entry.o._id).flat();
    jsTestLog(testLogPrefix + 'deleted doc _ids: ' + tojson(deletedDocIds));
    assert.sameMembers(deletedDocIds, docIds);

    rst.stopSet();
    jsTestLog(testLogPrefix + 'Test case finished successfully.');
}

runTest('latest', 'last-lts');
runTest('last-lts', 'latest');
})();
