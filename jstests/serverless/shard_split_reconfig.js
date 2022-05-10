/*
 * Test that shard split generates and apply a split config.
 *
 * @tags: [requires_fcv_52, featureFlagShardSplit, serverless]
 */

load("jstests/libs/fail_point_util.js");
load('jstests/libs/parallel_shell_helpers.js');
load("jstests/serverless/libs/basic_serverless_test.js");

function shardSplitApplySplitConfig() {
    "use strict";

    jsTestLog("Checking that shard split correctly reconfig the nodes.");

    // Skip db hash check because secondary is left with a different config.
    TestData.skipCheckDBHashes = true;
    const tenantIds = ["tenant1", "tenant2"];

    const test = new BasicServerlessTest({
        recipientTagName: "recipientNode",
        recipientSetName: "recipient",
        quickGarbageCollection: true
    });
    test.addRecipientNodes();

    const donorPrimary = test.donor.getPrimary();

    const operation = test.createSplitOperation(tenantIds);

    jsTestLog("Running commitShardSplit command");
    assert.commandWorked(operation.commit());

    jsTestLog("Asserting a split config has been applied");
    // We need to pass `nodeId` below because there might be 2 primaries available at the same time
    // from both the donor and the recipient nodes.
    const configDoc = test.donor.getReplSetConfigFromNode(donorPrimary.nodeId);
    assert(configDoc, "There must be a config document");
    assert.eq(configDoc["members"].length, 3);
    assert(configDoc["recipientConfig"]);
    assert.eq(configDoc["recipientConfig"]["_id"], "recipient");
    assert.eq(configDoc["recipientConfig"]["members"].length, 3);

    jsTestLog("Asserting state document exist after command");
    assertMigrationState(donorPrimary, operation.migrationId, "committed");

    operation.forget();

    test.waitForGarbageCollection(operation.migrationId, tenantIds);
    test.stop();
}

shardSplitApplySplitConfig();
