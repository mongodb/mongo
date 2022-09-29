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

function cannotStartMigrationWhenThereIsAnExistingAccessBlocker(protocol) {
    // Test that we cannot start a tenant migration for a tenant that already has an access blocker.
    const recipientTagName = "recipientTag";
    const recipientSetName = "recipient";
    const tenantIds = ["tenant1", "tenant2"];
    const splitMigrationId = UUID();
    const tenantMigrationId = UUID();

    // Ensure a high enough delay so the shard split document is not deleted before tenant migration
    // is started.
    sharedOptions = {};
    sharedOptions["setParameter"] = {
        shardSplitGarbageCollectionDelayMS: 36000000,
        ttlMonitorSleepSecs: 1
    };

    const test = new TenantMigrationTest({quickGarbageCollection: true, sharedOptions});

    let recipientNodes = addRecipientNodes(test.getDonorRst(), recipientTagName);

    const commitThread = commitSplitAsync(
        test.getDonorRst(), tenantIds, recipientTagName, recipientSetName, splitMigrationId);
    assert.commandWorked(commitThread.returnData());

    // Remove recipient nodes
    test.getDonorRst().nodes =
        test.getDonorRst().nodes.filter(node => !recipientNodes.includes(node));
    test.getDonorRst().ports =
        test.getDonorRst().ports.filter(port => !recipientNodes.some(node => node.port === port));

    assert.commandWorked(test.getDonorRst().getPrimary().adminCommand(
        {forgetShardSplit: 1, migrationId: splitMigrationId}));

    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(tenantMigrationId),
        protocol,
    };
    if (protocol != "shard merge") {
        migrationOpts["tenantId"] = tenantIds[0];
    }
    assert.commandFailed(test.startMigration(migrationOpts));

    recipientNodes.forEach(node => {
        MongoRunner.stopMongod(node);
    });

    test.stop();
    jsTestLog("cannotStartMigrationWhenThereIsAnExistingAccessBlocker test completed");
}

cannotStartMigrationWhenThereIsAnExistingAccessBlocker("multitenant migrations");
cannotStartMigrationWhenThereIsAnExistingAccessBlocker("shard merge");
