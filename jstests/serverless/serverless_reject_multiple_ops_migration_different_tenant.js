/**
 * @tags: [
 *   serverless,
 *   requires_fcv_62,
 *   featureFlagShardMerge
 * ]
 */

load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");
load("jstests/serverless/libs/shard_split_test.js");
load("jstests/libs/uuid_util.js");

function cannotStartMigrationWithDifferentTenantWhileShardSplitIsInProgress(protocol) {
    // Test that we cannot start a tenant migration while a shard split is in progress. Use a
    // tenantId uninvolved in the split.
    const recipientTagName = "recipientTag";
    const recipientSetName = "recipient";
    const tenantIds = ["tenant1", "tenant2"];
    const splitMigrationId = UUID();
    const tenantMigrationId = UUID();

    sharedOptions = {};
    sharedOptions["setParameter"] = {shardSplitGarbageCollectionDelayMS: 0, ttlMonitorSleepSecs: 1};

    const test = new TenantMigrationTest({quickGarbageCollection: true, sharedOptions});

    let recipientNodes = addRecipientNodes({rst: test.getDonorRst(), recipientTagName});

    let fp =
        configureFailPoint(test.getDonorRst().getPrimary(), "pauseShardSplitBeforeBlockingState");

    const commitThread = commitSplitAsync({
        rst: test.getDonorRst(),
        tenantIds,
        recipientTagName,
        recipientSetName,
        migrationId: splitMigrationId
    });
    fp.wait();

    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(tenantMigrationId),
        protocol,
    };
    if (protocol != "shard merge") {
        migrationOpts["tenantId"] = "otherTenantToMove";
    }
    jsTestLog("Starting tenant migration");
    assert.commandFailedWithCode(test.startMigration(migrationOpts),
                                 ErrorCodes.ConflictingServerlessOperation);

    fp.off();

    assert.commandWorked(commitThread.returnData());

    test.getDonorRst().nodes =
        test.getDonorRst().nodes.filter(node => !recipientNodes.includes(node));
    test.getDonorRst().ports =
        test.getDonorRst().ports.filter(port => !recipientNodes.some(node => node.port === port));

    assert.commandWorked(test.getDonorRst().getPrimary().adminCommand(
        {forgetShardSplit: 1, migrationId: splitMigrationId}));

    recipientNodes.forEach(node => {
        MongoRunner.stopMongod(node);
    });

    waitForGarbageCollectionForSplit(test.getDonorRst().nodes, splitMigrationId, tenantIds);

    test.stop();
    jsTestLog("cannotStartMigrationWithDifferentTenantWhileShardSplitIsInProgress test completed");
}

cannotStartMigrationWithDifferentTenantWhileShardSplitIsInProgress("multitenant migrations");
cannotStartMigrationWithDifferentTenantWhileShardSplitIsInProgress("shard merge");
