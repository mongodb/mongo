/**
 * Confirms that background index builds on a primary can be aborted using killop.
 * @tags: [requires_replication]
 */
(function() {
"use strict";

load('jstests/noPassthrough/libs/index_build.js');

const rst = new ReplSetTest({
    nodes: [
        {},
        {
            // Disallow elections on secondary.
            rsConfig: {
                priority: 0,
                votes: 0,
            },
        },
    ]
});
const nodes = rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const testDB = primary.getDB('test');
const coll = testDB.getCollection('test');

assert.commandWorked(coll.insert({a: 1}));

IndexBuildTest.pauseIndexBuilds(primary);

const createIdx =
    IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {a: 1}, {background: true});

// When the index build starts, find its op id.
const opId = IndexBuildTest.waitForIndexBuildToScanCollection(testDB, coll.getName(), 'a_1');

IndexBuildTest.assertIndexBuildCurrentOpContents(testDB, opId, (op) => {
    jsTestLog('Inspecting db.currentOp() entry for index build: ' + tojson(op));
    assert.eq(
        undefined,
        op.connectionId,
        'Was expecting IndexBuildsCoordinator op; found db.currentOp() for connection thread instead: ' +
            tojson(op));
    assert.eq(coll.getFullName(),
              op.ns,
              'Unexpected ns field value in db.currentOp() result for index build: ' + tojson(op));
});

// Kill the index builder thread.
assert.commandWorked(testDB.killOp(opId));

// Wait for the index build to stop from the killop signal.
try {
    IndexBuildTest.waitForIndexBuildToStop(testDB);
} finally {
    IndexBuildTest.resumeIndexBuilds(primary);
}

const exitCode = createIdx({checkExitSuccess: false});
assert.neq(0, exitCode, 'expected shell to exit abnormally due to index build being terminated');

// Check that no new index has been created.  This verifies that the index build was aborted
// rather than successfully completed.
IndexBuildTest.assertIndexes(coll, 1, ['_id_']);

// Two-phase index builds replicate different oplog entries.
if (IndexBuildTest.supportsTwoPhaseIndexBuild(primary)) {
    const cmdNs = testDB.getCollection('$cmd').getFullName();
    let ops = rst.dumpOplog(primary, {op: 'c', ns: cmdNs, 'o.startIndexBuild': coll.getName()});
    assert.eq(1, ops.length, 'incorrect number of startIndexBuild oplog entries: ' + tojson(ops));
    ops = rst.dumpOplog(primary, {op: 'c', ns: cmdNs, 'o.abortIndexBuild': coll.getName()});
    assert.eq(1, ops.length, 'incorrect number of abortIndexBuild oplog entries: ' + tojson(ops));
    ops = rst.dumpOplog(primary, {op: 'c', ns: cmdNs, 'o.commitIndexBuild': coll.getName()});
    assert.eq(0, ops.length, 'incorrect number of commitIndexBuild oplog entries: ' + tojson(ops));
} else {
    // The noop oplog entry is the only evidence of the failed single phase index build.
    let ops = rst.dumpOplog(
        primary, {op: 'n', ns: '', 'o.msg': 'Creating indexes. Coll: ' + coll.getFullName()});
    assert.eq(1, ops.length, 'incorrect number of noop oplog entries: ' + tojson(ops));
}
rst.stopSet();
})();
