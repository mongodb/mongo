/**
 * Tests the 'voteAbortIndexBuild' internal command.
 *
 * @tags: [
 *   featureFlagIndexBuildGracefulErrorHandling,
 *   requires_replication,
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
            },
        },
        {/* arbiter */ rsConfig: {arbiterOnly: true}}
    ]
});
const nodes = rst.startSet();
rst.initiate();

const primary = rst.getPrimary();

const testDB = primary.getDB('test');
const coll = testDB.getCollection('test');

// Insert one document to avoid empty collection optimization.
assert.commandWorked(coll.insert({a: 1}));

// Pause the index build, using the 'hangAfterStartingIndexBuild' failpoint.
IndexBuildTest.pauseIndexBuilds(primary);

const createIdx = IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {'a': 1});

// Wait for the index build to start on the primary.
const opId = IndexBuildTest.waitForIndexBuildToStart(testDB, coll.getName(), 'a_1');
IndexBuildTest.assertIndexBuildCurrentOpContents(testDB, opId);

// Extract the index build UUID.
const buildUUID =
    IndexBuildTest.assertIndexesSoon(coll, 2, ['_id_'], ['a_1'], {includeBuildUUIDs: true})['a_1']
        .buildUUID;

const abortReason = jsTestName();

// Running 'voteAbortIndexBuild' as an arbiter node should fail.
assert.commandFailedWithCode(primary.adminCommand({
    voteAbortIndexBuild: buildUUID,
    hostAndPort: rst.getArbiter().host,
    reason: abortReason,
    writeConcern: {w: "majority"}
}),
                             7329201);

IndexBuildTest.assertIndexes(coll, 2, ['_id_'], ['a_1']);

// Running 'voteAbortIndexBuild' as a non-member node should fail. Use Doom's reserved port.
const invalidHostAndPort = "localhost:666";
assert.commandFailedWithCode(primary.adminCommand({
    voteAbortIndexBuild: buildUUID,
    hostAndPort: invalidHostAndPort,
    reason: abortReason,
    writeConcern: {w: "majority"}
}),
                             7329201);

IndexBuildTest.assertIndexes(coll, 2, ['_id_'], ['a_1']);

// Running 'voteAbortIndexBuild' as data-bearing secondary, should fail due to missing reason.
assert.commandFailedWithCode(primary.adminCommand({
    voteAbortIndexBuild: buildUUID,
    hostAndPort: rst.getSecondary().host,
    writeConcern: {w: "majority"}
}),
                             40414);

// Running 'voteAbortIndexBuild' as data-bearing secondary, should succeed.
assert.commandWorked(primary.adminCommand({
    voteAbortIndexBuild: buildUUID,
    hostAndPort: rst.getSecondary().host,
    reason: abortReason,
    writeConcern: {w: "majority"}
}));

// Wait for the index build to stop.
const exitCode = createIdx({checkExitSuccess: false});
assert.neq(0, exitCode, 'expected shell to exit abnormally due to index build abort');

// Verify none of the nodes list the aborted index, and all of them have replicated the
// 'abortIndexBuild' oplog entry.
for (const node of rst.nodes) {
    // Skip arbiter.
    if (node.getDB('admin')._helloOrLegacyHello().arbiterOnly) {
        continue;
    }

    const nodeDB = node.getDB(testDB.getName());
    const nodeColl = nodeDB.getCollection(coll.getName());

    IndexBuildTest.assertIndexes(nodeColl, 1, ['_id_']);

    const cmdNs = nodeDB.getCollection('$cmd').getFullName();
    const ops = rst.dumpOplog(node, {op: 'c', ns: cmdNs, 'o.abortIndexBuild': coll.getName()});
    assert.eq(1, ops.length, 'abortIndexBuild oplog entry not found: ' + tojson(ops));

    // Verify the abort reason is replicated in the oplog.
    const replicatedErrorMsg = ops[0].o.cause.errmsg;
    assert.neq(-1, replicatedErrorMsg.indexOf(abortReason));
}

// Re-issuing 'voteAbortIndexBuild' command on already aborted build should fail.
assert.commandFailedWithCode(primary.adminCommand({
    voteAbortIndexBuild: buildUUID,
    hostAndPort: rst.getSecondary().host,
    reason: abortReason,
    writeConcern: {w: "majority"}
}),
                             7329202);

rst.stopSet();
})();
