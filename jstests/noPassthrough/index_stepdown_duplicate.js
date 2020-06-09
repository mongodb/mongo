/**
 * Confirms that aborted index builds on a stepped down primary do not conflict with subsequent
 * index builds replicated from the new primary with the same index names.
 * @tags: [
 *     requires_replication,
 * ]
 */
(function() {
"use strict";

load('jstests/libs/logv2_helpers.js');
load('jstests/noPassthrough/libs/index_build.js');

const rst = new ReplSetTest({
    // We want at least two electable nodes.
    nodes: [{}, {}, {arbiter: true}],
});
const nodes = rst.startSet();
rst.initiate();

const primary = rst.getPrimary();

// This test requires index builds to start on the createIndexes oplog entry and expects
// index builds to be interrupted when the primary steps down.
if (IndexBuildTest.supportsTwoPhaseIndexBuild(primary)) {
    jsTestLog('Two phase index builds not supported, skipping test.');
    rst.stopSet();
    return;
}

let coll = primary.getCollection('test.test');

assert.commandWorked(coll.insert({a: 1}));

assert.commandWorked(primary.adminCommand(
    {configureFailPoint: 'hangAfterInitializingIndexBuild', mode: 'alwaysOn'}));

// Use a custom index name because we are going to reuse it later with a different key pattern.
const createIdx =
    IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {a: 1}, {name: 'myidx'});
if (isJsonLog(primary)) {
    checkLog.containsJson(primary, 20384, {
        namespace: coll.getFullName(),
        properties: (desc) => {
            return desc.name === 'myidx';
        },
    });
} else {
    checkLog.contains(primary,
                      new RegExp('index build: starting.*' + coll.getFullName() + '.*myidx'));
}

const newPrimary = rst.getSecondary();

// Step down the primary. Confirm that the index build was aborted.
assert.commandWorked(primary.adminCommand({replSetStepDown: 60, force: true}));

// Let the build finish aborting
assert.commandWorked(
    primary.adminCommand({configureFailPoint: 'hangAfterInitializingIndexBuild', mode: 'off'}));
const exitCode = createIdx({checkExitSuccess: false});
assert.neq(0, exitCode, 'expected shell to exit abnormally due to index build being terminated');
checkLog.containsJson(primary, 20443);

// Build same index on new primary. This should not crash the old primary when replicated.
rst.stepUp(newPrimary);
const collOnNewPrimary = newPrimary.getCollection(coll.getFullName());
assert.commandWorked(collOnNewPrimary.createIndex({b: 1}, {name: 'myidx'}));
let indexSpecMap = IndexBuildTest.assertIndexes(collOnNewPrimary, 2, ['_id_', 'myidx']);
assert.eq({b: 1}, indexSpecMap['myidx'].key, 'unexpected key pattern: ' + tojson(indexSpecMap));

rst.awaitReplication();

// Even though the first index build was aborted on the stepped down primary, the new index build
// started on the new primary should still be successfully replicated.
// Check the old primary.
coll = primary.getCollection(coll.getFullName());
indexSpecMap = IndexBuildTest.assertIndexes(coll, 2, ['_id_', 'myidx']);
assert.eq({b: 1}, indexSpecMap['myidx'].key, 'unexpected key pattern: ' + tojson(indexSpecMap));

// Check the new primary.
coll = newPrimary.getCollection(coll.getFullName());
indexSpecMap = IndexBuildTest.assertIndexes(coll, 2, ['_id_', 'myidx']);
assert.eq({b: 1}, indexSpecMap['myidx'].key, 'unexpected key pattern: ' + tojson(indexSpecMap));

rst.stopSet();
})();
