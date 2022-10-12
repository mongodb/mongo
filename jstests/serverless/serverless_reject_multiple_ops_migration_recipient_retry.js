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

function cannotStartMigrationWhileShardSplitIsInProgressOnRecipient(protocol) {
    // Test that we cannot start a migration while a shard split is in progress.
    const recipientTagName = "recipientTag";
    const recipientSetName = "recipient";
    const tenantIds = ["tenant1", "tenant2"];
    const splitMigrationId = UUID();
    const tenantMigrationId = UUID();
    const secondTenantMigrationId = UUID();

    sharedOptions = {};
    sharedOptions["setParameter"] = {shardSplitGarbageCollectionDelayMS: 0, ttlMonitorSleepSecs: 1};

    const test = new TenantMigrationTest({quickGarbageCollection: true, sharedOptions});

    const splitRst = test.getRecipientRst();

    let splitRecipientNodes = addRecipientNodes({rst: splitRst, recipientTagName});

    let fp = configureFailPoint(splitRst.getPrimary(), "pauseShardSplitBeforeBlockingState");

    const commitThread = commitSplitAsync({
        rst: splitRst,
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
        migrationOpts["tenantId"] = tenantIds[0];
    }
    jsTestLog("Starting tenant migration");
    assert.commandWorked(test.startMigration(migrationOpts));

    const result = assert.commandWorked(test.waitForMigrationToComplete(migrationOpts));
    assert.eq(result.state, "aborted");
    assert.eq(result.abortReason.code, ErrorCodes.ConflictingServerlessOperation);
    assert.commandWorked(test.forgetMigration(migrationOpts.migrationIdString));

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

    const secondMigrationOpts = {
        migrationIdString: extractUUIDFromObject(secondTenantMigrationId),
        protocol,
    };
    if (protocol != "shard merge") {
        secondMigrationOpts["tenantId"] = tenantIds[0];
    }
    jsTestLog("Starting tenant migration");
    assert.commandWorked(test.startMigration(secondMigrationOpts));
    TenantMigrationTest.assertCommitted(test.waitForMigrationToComplete(secondMigrationOpts));
    assert.commandWorked(test.forgetMigration(secondMigrationOpts.migrationIdString));

    waitForGarbageCollectionForSplit(splitRst.nodes, splitMigrationId, tenantIds);

    test.stop();
    jsTestLog("cannotStartMigrationWhileShardSplitIsInProgressOnRecipient test completed");
}

cannotStartMigrationWhileShardSplitIsInProgressOnRecipient("multitenant migrations");
cannotStartMigrationWhileShardSplitIsInProgressOnRecipient("shard merge");
