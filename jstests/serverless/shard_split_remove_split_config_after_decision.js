/*
 * Test that a split config is removed after a decision is reached.
 *
 * @tags: [requires_fcv_62, serverless]
 */

load("jstests/libs/fail_point_util.js");
load("jstests/serverless/libs/shard_split_test.js");

function assertSplitConfigExists(donorPrimary) {
    jsTestLog("Asserting a split config has been applied");
    const config = assert.commandWorked(donorPrimary.adminCommand({replSetGetConfig: 1})).config;
    assert(config, "There must be a config document");
    assert.eq(config["members"].length, 3);
    assert(config["recipientConfig"]);
    assert.eq(config["recipientConfig"]["_id"], "recipient");
    assert.eq(config["recipientConfig"]["members"].length, 3);
}

function assertSplitConfigDoesNotExist(donorPrimary) {
    jsTestLog("Asserting a split config does not exist");
    const config = assert.commandWorked(donorPrimary.adminCommand({replSetGetConfig: 1})).config;
    assert(config, "There must be a config document");
    assert.eq(null, config["recipientConfig"]);
}

function splitConfigRemovedAfterDecision(simulateErrorToAbortOperation) {
    "use strict";

    simulateErrorToAbortOperation = simulateErrorToAbortOperation || false;

    // Skip db hash check because secondary is left with a different config.
    TestData.skipCheckDBHashes = true;
    const tenantIds = ["tenant1", "tenant2"];
    const test = new ShardSplitTest({
        recipientTagName: "recipientNode",
        recipientSetName: "recipient",
        quickGarbageCollection: true
    });

    test.addRecipientNodes();

    const donorPrimary = test.donor.getPrimary();
    const beforeConfigRemovalFp =
        configureFailPoint(donorPrimary, "pauseShardSplitBeforeSplitConfigRemoval");
    const afterDecisionFp = configureFailPoint(donorPrimary, "pauseShardSplitAfterDecision");
    if (simulateErrorToAbortOperation) {
        configureFailPoint(donorPrimary, "abortShardSplitBeforeLeavingBlockingState");
    }

    const operation = test.createSplitOperation(tenantIds);
    if (simulateErrorToAbortOperation) {
        assert.commandFailed(operation.commit());
    } else {
        assert.commandWorked(operation.commit());
    }

    beforeConfigRemovalFp.wait();
    if (simulateErrorToAbortOperation) {
        assertMigrationState(donorPrimary, operation.migrationId, "aborted");
    } else {
        assertMigrationState(donorPrimary, operation.migrationId, "committed");
    }

    assertSplitConfigExists(donorPrimary);
    beforeConfigRemovalFp.off();

    afterDecisionFp.wait();
    assertSplitConfigDoesNotExist(donorPrimary);
    afterDecisionFp.off();

    operation.forget();
    test.waitForGarbageCollection(operation.migrationId, tenantIds);
    test.stop();
}

splitConfigRemovedAfterDecision(false);
splitConfigRemovedAfterDecision(true);
