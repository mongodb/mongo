/**
 * @tags: [
 *   serverless,
 *   requires_fcv_52,
 *   featureFlagShardSplit,
 *   featureFlagShardMerge
 * ]
 */

load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");
load("jstests/serverless/libs/basic_serverless_test.js");
load("jstests/serverless/libs/serverless_reject_multiple_ops_utils.js");
load("jstests/libs/uuid_util.js");

function cannotStartMigrationWhileShardSplitIsInProgress(protocol) {
    // Test that we cannot start a migration while a shard split is in progress.
    const recipientTagName = "recipientTag";
    const recipientSetName = "recipient";
    const tenantIds = ["tenant1", "tenant2"];
    const splitMigrationId = UUID();
    const tenantMigrationId = UUID();

    sharedOptions = {};
    sharedOptions["setParameter"] = {shardSplitGarbageCollectionDelayMS: 0, ttlMonitorSleepSecs: 1};

    const test = new TenantMigrationTest({quickGarbageCollection: true, sharedOptions});

    const splitRst = test.getDonorRst();

    let splitRecipientNodes = addRecipientNodes(splitRst, recipientTagName);

    let fp = configureFailPoint(splitRst.getPrimary(), "pauseShardSplitBeforeBlockingState");

    const commitThread =
        commitSplitAsync(splitRst, tenantIds, recipientTagName, recipientSetName, splitMigrationId);
    fp.wait();

    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(tenantMigrationId),
        protocol,
    };
    if (protocol != "shard merge") {
        migrationOpts["tenantId"] = tenantIds[0];
    }
    jsTestLog("Starting tenant migration");
    assert.commandFailedWithCode(test.startMigration(migrationOpts),
                                 ErrorCodes.ConflictingServerlessOperation);

    fp.off();

    assert.commandWorked(commitThread.returnData());

    splitRst.nodes = splitRst.nodes.filter(node => !splitRecipientNodes.includes(node));
    splitRst.ports =
        splitRst.ports.filter(port => !splitRecipientNodes.some(node => node.port === port));

    assert.commandWorked(
        splitRst.getPrimary().adminCommand({forgetShardSplit: 1, migrationId: splitMigrationId}));

    splitRecipientNodes.forEach(node => {
        MongoRunner.stopMongod(node);
    });

    waitForGarbageCollectionForSplit(splitRst.nodes, splitMigrationId, tenantIds);

    test.stop();
    jsTestLog("cannotStartMigrationWhileShardSplitIsInProgress test completed");
}

cannotStartMigrationWhileShardSplitIsInProgress("multitenant migrations");
cannotStartMigrationWhileShardSplitIsInProgress("shard merge");
