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
load("jstests/libs/parallelTester.js");
load("jstests/libs/uuid_util.js");

function canStartMigrationAfterSplitGarbageCollection(protocol) {
    // Test that we can start a migration after a shard split has been garbage collected.
    const recipientTagName = "recipientTag";
    const recipientSetName = "recipient";
    const tenantIds = ["tenant1", "tenant2"];
    const splitMigrationId = UUID();
    const tenantMigrationId = UUID();

    sharedOptions = {};
    sharedOptions["setParameter"] = {shardSplitGarbageCollectionDelayMS: 0, ttlMonitorSleepSecs: 1};

    const test = new TenantMigrationTest({quickGarbageCollection: true, sharedOptions});

    let recipientNodes = addRecipientNodes({rst: test.getDonorRst(), recipientTagName});

    const commitThread = commitSplitAsync({
        rst: test.getDonorRst(),
        tenantIds,
        recipientTagName,
        recipientSetName,
        migrationId: splitMigrationId
    });
    assert.commandWorked(commitThread.returnData());

    // Remove recipient nodes
    test.getDonorRst().nodes =
        test.getDonorRst().nodes.filter(node => !recipientNodes.includes(node));
    test.getDonorRst().ports =
        test.getDonorRst().ports.filter(port => !recipientNodes.some(node => node.port === port));

    assert.commandWorked(test.getDonorRst().getPrimary().adminCommand(
        {forgetShardSplit: 1, migrationId: splitMigrationId}));

    waitForGarbageCollectionForSplit(test.getDonorRst().nodes, splitMigrationId, tenantIds);

    jsTestLog("Starting tenant migration");
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(tenantMigrationId),
        protocol,
    };
    if (protocol != "shard merge") {
        migrationOpts["tenantId"] = tenantIds[0];
    }
    assert.commandWorked(test.startMigration(migrationOpts));

    TenantMigrationTest.assertCommitted(test.waitForMigrationToComplete(migrationOpts));
    assert.commandWorked(test.forgetMigration(migrationOpts.migrationIdString));

    recipientNodes.forEach(node => {
        MongoRunner.stopMongod(node);
    });

    test.stop();
    jsTestLog("canStartMigrationAfterSplitGarbageCollection test completed");
}

canStartMigrationAfterSplitGarbageCollection("multitenant migrations");
canStartMigrationAfterSplitGarbageCollection("shard merge");
