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

const newPrimary = rst.getSecondary();

// Step down the primary. Confirm that the index build was aborted before we trigger a crash in the
// primary.
assert.commandWorked(primary.adminCommand({replSetStepDown: 60, force: true}));
const exitCode = createIdx({checkExitSuccess: false});
assert.neq(0, exitCode, 'expected shell to exit abnormally due to index build being terminated');
checkLog.contains(primary, 'Index build aborted: ');

// Build same index on new primary. This will crash the old primary when replicated.
rst.stepUp(newPrimary);
const collOnNewPrimary = newPrimary.getCollection(coll.getFullName());
assert.commandWorked(collOnNewPrimary.createIndex({b: 1}, {name: 'myidx'}));
let indexSpecMap = IndexBuildTest.assertIndexes(collOnNewPrimary, 2, ['_id_', 'myidx']);
assert.eq({b: 1}, indexSpecMap['myidx'].key, 'unexpected key pattern: ' + tojson(indexSpecMap));

// Old primary should have aborted on trying to process the new createIndexes oplog entry while
// an aborted index build, left over from before the node stepped down, is still running.
assert.soon(() => {
    return rawMongoProgramOutput().match(
        'Fatal assertion 34437 IndexBuildAborted: Index build conflict:');
}, 'Index build should have aborted on step down.');
rst.stop(primary, undefined, {allowedExitCode: MongoRunner.EXIT_ABRUPT});

// Restart old primary so that it will try to process the failed createIndexes oplog entry
// from the new primary that we stepped up.
// Restarting the node invalidates 'coll' so we need to resolve the Collection object
// using the connection to the restarted node.
const oldPrimary = rst.start(primary, undefined, true);
coll = oldPrimary.getCollection(coll.getFullName());
rst.awaitReplication();

// Even though the first index build was aborted on the stepped down primary, the new index build
// started on the new primary should still be successfully replicated.
indexSpecMap = IndexBuildTest.assertIndexes(coll, 2, ['_id_', 'myidx']);
assert.eq({b: 1}, indexSpecMap['myidx'].key, 'unexpected key pattern: ' + tojson(indexSpecMap));

rst.stopSet();
})();
