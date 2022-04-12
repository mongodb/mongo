/**
 *
 * Tests that we can't run concurrent shard splits.
 * @tags: [requires_fcv_52, featureFlagShardSplit]
 */

load("jstests/libs/fail_point_util.js");         // for "configureFailPoint"
load('jstests/libs/parallel_shell_helpers.js');  // for "startParallelShell"
load("jstests/serverless/libs/basic_serverless_test.js");

(function() {
"use strict";

const recipientTagName = "recipientNode";
const recipientSetName = "recipientSetName";
const tenantIds = ["tenant1", "tenant2"];

function commitShardSplitConcurrently() {
    const test =
        new BasicServerlessTest({recipientTagName, recipientSetName, quickGarbageCollection: true});
    test.addRecipientNodes();

    const migrationId = UUID();

    const donorPrimary = test.donor.getPrimary();

    let fp = configureFailPoint(donorPrimary.getDB("admin"), "pauseShardSplitAfterBlocking");

    const awaitSecondSplitOperation = startParallelShell(
        funWithArgs(
            function(migrationId, recipientTagName, recipientSetName, tenantIds) {
                assert.commandWorked(db.adminCommand({
                    commitShardSplit: 1,
                    migrationId,
                    recipientTagName,
                    recipientSetName,
                    tenantIds
                }));
            },
            migrationId,
            test.recipientTagName,
            test.recipientSetName,
            tenantIds),
        donorPrimary.port);

    fp.wait();

    // fails because there is an ongoing shard split in blocking state.
    assert.commandFailedWithCode(donorPrimary.adminCommand({
        commitShardSplit: 1,
        migrationId: UUID(),
        tenantIds: ["tenant3", "tenant4"],
        recipientTagName: test.recipientTagName,
        recipientSetName: test.recipientSetName
    }),
                                 117);  // ConflictingOperationInProgress

    fp.off();

    awaitSecondSplitOperation();

    // fails because there is an ongoing shard split that's about to complete.
    assert.commandFailedWithCode(donorPrimary.adminCommand({
        commitShardSplit: 1,
        migrationId: UUID(),
        tenantIds: ["tenant8", "tenant9"],
        recipientTagName: test.recipientTagName,
        recipientSetName: test.recipientSetName
    }),
                                 117);  // ConflictingOperationInProgress

    test.removeRecipientNodesFromDonor();
    assert.commandWorked(
        donorPrimary.adminCommand({forgetShardSplit: 1, migrationId: migrationId}));

    // fails because the commitShardSplit hasn't be garbage collected yet.
    assert.commandFailedWithCode(donorPrimary.adminCommand({
        commitShardSplit: 1,
        migrationId: UUID(),
        tenantIds: ["tenant1", "tenant3"],  // re use one tenantid
        recipientTagName: test.recipientTagName,
        recipientSetName: test.recipientSetName
    }),
                                 117);  // ConflictingOperationInProgress

    test.waitForGarbageCollection(migrationId, tenantIds);

    // another split operation can start after garbage collection of the previous one.
    assert.commandWorked(donorPrimary.adminCommand({
        commitShardSplit: 1,
        migrationId: UUID(),
        tenantIds: ["tenant10", "tenant11"],
        recipientTagName: test.recipientTagName,
        recipientSetName: test.recipientSetName
    }));

    test.stop();
}

function commitShardSplitAfterAbort() {
    const test = new BasicServerlessTest({
        recipientTagName,
        recipientSetName,
        nodeOptions: {
            // Set a short timeout to test that the operation times out waiting for replication
            setParameter: "shardSplitTimeoutMS=2000"
        }
    });
    test.addRecipientNodes();

    const migrationId = UUID();
    const admin = test.donor.getPrimary().getDB("admin");
    let fp = configureFailPoint(admin, "pauseShardSplitAfterBlocking");

    assert.commandFailed(admin.runCommand(
        {commitShardSplit: 1, migrationId, recipientTagName, recipientSetName, tenantIds}));

    fp.wait();

    // fails because the commitShardSplit hasn't be garbage collected yet.
    assert.commandFailedWithCode(admin.runCommand({
        commitShardSplit: 1,
        migrationId: UUID(),
        tenantIds: tenantIds,
        recipientTagName: test.recipientTagName,
        recipientSetName: test.recipientSetName
    }),
                                 117);  // ConflictingOperationInProgress

    test.stop();
}

commitShardSplitConcurrently();
commitShardSplitAfterAbort();
})();
