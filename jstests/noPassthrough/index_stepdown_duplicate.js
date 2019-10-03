/**
 * Confirms that aborted index builds on a stepped down primary do not conflict with subsequent
 * index builds replicated from the new primary with the same index names.
 * @tags: [
 *     requires_replication,
 *     two_phase_index_builds_unsupported,
 * ]
 */
(function() {
"use strict";

load('jstests/libs/check_log.js');
load('jstests/noPassthrough/libs/index_build.js');

const rst = new ReplSetTest({
    // We want at least two electable nodes.
    nodes: [{}, {}, {arbiter: true}],
});
const nodes = rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
let coll = primary.getCollection('test.test');

assert.commandWorked(coll.insert({a: 1}));

assert.commandWorked(primary.adminCommand(
    {configureFailPoint: 'hangAfterInitializingIndexBuild', mode: 'alwaysOn'}));

// Use a custom index name because we are going to reuse it later with a different key pattern.
const createIdx =
    IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {a: 1}, {name: 'myidx'});

checkLog.contains(
    primary, 'index build: starting on ' + coll.getFullName() + ' properties: { v: 2, key: { a:');

let newPrimary = rst.getSecondary();
try {
    // Step down the primary.
    assert.commandWorked(primary.adminCommand({replSetStepDown: 60, force: true}));

    // Build same index on new primary.
    rst.stepUp(newPrimary);
    const collOnNewPrimary = newPrimary.getCollection(coll.getFullName());
    assert.commandWorked(collOnNewPrimary.createIndex({b: 1}, {name: 'myidx'}));
    let indexSpecMap = IndexBuildTest.assertIndexes(collOnNewPrimary, 2, ['_id_', 'myidx']);
    assert.eq({b: 1}, indexSpecMap['myidx'].key, 'unexpected key pattern: ' + tojson(indexSpecMap));
} finally {
    assert.commandWorked(
        primary.adminCommand({configureFailPoint: 'hangAfterInitializingIndexBuild', mode: 'off'}));
}

// Wait for the index build to stop.
IndexBuildTest.waitForIndexBuildToStop(coll.getDB());

const exitCode = createIdx({checkExitSuccess: false});
assert.neq(0, exitCode, 'expected shell to exit abnormally due to index build being terminated');

checkLog.contains(primary, 'IndexBuildAborted: Index build aborted: ');

// Even though the first index build was aborted on the stepped down primary, the new index build
// started on the new primary should still be successfully replicated.
// Unfortunately, if the aborted index build is still present, the new createIndexes oplog entry
// will be ignored with a non-fatal IndexBuildAlreadyIn Progress error.
IndexBuildTest.assertIndexes(coll, 1, ['_id_']);
TestData.skipCheckDBHashes = true;

rst.stopSet();
})();
