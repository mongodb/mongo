/**
 * Test that the impersonated user metadata, client metadata, and cluster time are propagated to the
 * primary when a $out/$merge is executed on a secondary.
 *
 * @tags: [
 *   assumes_unsharded_collection,
 *   requires_replication,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");

const replTest = new ReplSetTest({nodes: 2});
replTest.startSet();
replTest.initiate();
replTest.awaitReplication();

const primary = replTest.getPrimary();
const secondary = replTest.getSecondary();
const primaryDB = primary.getDB("test");
const secondaryDB = secondary.getDB("test");
assert.commandWorked(primaryDB.setProfilingLevel(2));
assert.commandWorked(secondaryDB.setProfilingLevel(2));
secondaryDB.getMongo().setReadPref("secondary");

const outCollName = "outColl";
const inputCollPrimary = primaryDB.getCollection("inputColl");

assert.commandWorked(inputCollPrimary.insert({_id: 0, a: 1}, {writeConcern: {w: 2}}));
assert.commandWorked(inputCollPrimary.insert({_id: 1, a: 2}, {writeConcern: {w: 2}}));

assert.commandWorked(
    primaryDB.runCommand({createUser: "testUser", pwd: "pwd", roles: [], writeConcern: {w: 2}}));
primaryDB.grantRolesToUser("testUser", [{role: "readWrite", db: "test"}], {w: 2});
const expectedUserAndRoleMetadata = {
    $impersonatedUser: {"user": "testUser", "db": "test"},
    $impersonatedRoles: [{"role": "readWrite", "db": "test"}]
};

function testMetadata(pipeline, comment) {
    configureFailPoint(primary, "hangDuringBatchInsert");

    const runAggregate = `
            const testDB = db.getSiblingDB("test");
            assert.eq(testDB.auth("testUser", "pwd"), 1);
            const res = testDB.runCommand({
                aggregate: "inputColl",
                pipeline: ${tojson(pipeline)},
                writeConcern: {w: 2},
                comment: "${comment}",
                cursor: {},
                $readPreference: {mode: "secondary"}
            });
            assert.commandWorked(res);
        `;
    let awaitShell = startParallelShell(runAggregate, secondary.port);

    // Get the client metadata and cluster time values from the secondary that we expect to be
    // propagated to the primary.
    let expectedClientMetadata = {};
    let secondaryClusterTime = {};
    assert.soon(() => {
        const curOps =
            secondary.getDB("admin")
                .aggregate([{$currentOp: {allUsers: true}}, {$match: {"command.comment": comment}}])
                .toArray();
        if (curOps.length === 0) {
            return false;
        }
        assert.eq(curOps.length, 1);
        expectedClientMetadata = curOps[0].clientMetadata;
        secondaryClusterTime = curOps[0].command.$clusterTime.clusterTime;
        return true;
    });

    assert.commandWorked(
        primary.adminCommand({configureFailPoint: "hangDuringBatchInsert", mode: "off"}));
    awaitShell();

    // Assert that the impersonated user metadata, client metadata, and cluster time are propagated
    // to the primary.
    const profile =
        primaryDB.system.profile.find({"command.comment": comment}).hint({$natural: 1}).toArray();
    let prevClusterTime = undefined;
    profile.forEach(op => {
        assert.eq(op.command.$audit, expectedUserAndRoleMetadata);
        assert.eq(op.command.$client, expectedClientMetadata);

        // If this was a $out, then there will be multiple entries due to the temporary collection
        // creation and rename. The first entry should have at least the cluster time that was
        // propagated from the secondary, while each subsequent entry should have a strictly
        // increasing cluster time.
        if (prevClusterTime === undefined) {
            assert.gte(op.command.$clusterTime.clusterTime, secondaryClusterTime);
        } else {
            assert.gt(op.command.$clusterTime.clusterTime, prevClusterTime);
        }
        prevClusterTime = op.command.$clusterTime.clusterTime;
    });

    // Drop the output collection to ensure that the drop replicates to the secondary. This
    // ensures that any state created by this test is cleaned up in preparation for later test
    // cases.
    let outColl = primaryDB.getCollection(outCollName);
    outColl.drop({writeConcern: {w: 2}});
}

const mergePipeline =
    [{$merge: {into: outCollName, whenMatched: "fail", whenNotMatched: "insert"}}];
testMetadata(mergePipeline, "merge_on_secondary_metadata");

const outPipeline = [{$group: {_id: "$_id", sum: {$sum: "$a"}}}, {$out: outCollName}];
testMetadata(outPipeline, "out_on_secondary_metadata");

replTest.stopSet();
})();
