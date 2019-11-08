/**
 * Confirms that index builds on a secondary are aborted when we encounter a document that cannot be
 * indexed. Since we expect the index build on the primary to fail, the secondary should wait for
 * the primary's abortIndexBuild oplog entry.
 * @tags: [
 *     requires_replication,
 * ]
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

// This test requires index builds to start on the createIndexes oplog entry and expects index
// builds to be interrupted when the primary steps down.
if (!IndexBuildTest.supportsTwoPhaseIndexBuild(primary)) {
    jsTestLog('Two phase index builds not supported, skipping test.');
    rst.stopSet();
    return;
}

const testDB = primary.getDB('test');
const coll = testDB.getCollection('test');

assert.commandWorked(coll.insert({a: 1}));

// There are two 'a.0' fields in this doc, which prevents us from creating an index on {'a.0': 1}.
// Inserting this document causes the index build to fail during the collection scan phase.
const invalidDocForIndex = {
    a: [
        {'0': 1},
    ],
};

assert.commandWorked(coll.insert(invalidDocForIndex));

// We are using this fail point to pause the index build before it starts the collection scan.
// This is important for this test because we want to prevent the index build on the primary from
// observing the invalid document while we block its progress.
assert.commandWorked(
    testDB.adminCommand({configureFailPoint: 'hangAfterSettingUpIndexBuild', mode: 'alwaysOn'}));

const createIdx = IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {'a.0': 1});

const secondary = rst.getSecondary();
try {
    // Wait for the index build to start on the primary. We cannot check the secondary because it
    // may have crashed by then due to the invalid document.
    const opId = IndexBuildTest.waitForIndexBuildToStart(testDB, coll.getName(), 'a.0_1');
    IndexBuildTest.assertIndexBuildCurrentOpContents(testDB, opId);

    const fassertProcessExitCode = _isWindows() ? MongoRunner.EXIT_ABRUPT : MongoRunner.EXIT_ABORT;
    rst.stop(secondary, undefined, {allowedExitCode: fassertProcessExitCode});
    assert(rawMongoProgramOutput().match('Fatal assertion 51101 Location16746: Index build: '),
           'Index build should have aborted on error.');
} finally {
    testDB.adminCommand({configureFailPoint: 'hangAfterSettingUpIndexBuild', mode: 'off'});
}

// Wait for the index build to stop.
IndexBuildTest.waitForIndexBuildToStop(testDB);

const exitCode = createIdx({checkExitSuccess: false});
assert.neq(0, exitCode, 'expected shell to exit abnormally due to index build failing');

// Check indexes on primary.
IndexBuildTest.assertIndexes(coll, 1, ['_id_']);

const cmdNs = testDB.getCollection('$cmd').getFullName();
const ops = rst.dumpOplog(primary, {op: 'c', ns: cmdNs, 'o.abortIndexBuild': coll.getName()});
assert.eq(1, ops.length, 'primary did not write abortIndexBuild oplog entry: ' + tojson(ops));

TestData.skipCheckDBHashes = true;
rst.stopSet();
})();
