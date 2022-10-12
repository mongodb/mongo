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

function canStartShardSplitWithAbortedMigration({protocol, runOnRecipient}) {
    const recipientTagName = "recipientTag";
    const recipientSetName = "recipient";
    const tenantIds = ["tenant1", "tenant2"];
    const splitMigrationId = UUID();
    const tenantMigrationId = UUID();

    sharedOptions = {};
    sharedOptions["setParameter"] = {shardSplitGarbageCollectionDelayMS: 0, ttlMonitorSleepSecs: 1};

    const test = new TenantMigrationTest({quickGarbageCollection: false, sharedOptions});

    const shardSplitRst = runOnRecipient ? test.getRecipientRst() : test.getDonorRst();

    let recipientNodes = addRecipientNodes({rst: test.getDonorRst(), recipientTagName});

    let fp = configureFailPoint(test.getDonorRst().getPrimary(),
                                "abortTenantMigrationBeforeLeavingBlockingState");
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(tenantMigrationId),
        protocol,
    };
    if (protocol != "shard merge") {
        migrationOpts["tenantId"] = tenantIds[0];
    }
    jsTestLog("Starting tenant migration");
    assert.commandWorked(test.startMigration(migrationOpts));

    TenantMigrationTest.assertAborted(test.waitForMigrationToComplete(migrationOpts));
    assert.commandWorked(test.forgetMigration(migrationOpts.migrationIdString));

    const commitThread = commitSplitAsync({
        rst: shardSplitRst,
        tenantIds,
        recipientTagName,
        recipientSetName,
        migrationId: splitMigrationId
    });
    assert.commandWorked(commitThread.returnData());

    recipientNodes.forEach(node => {
        MongoRunner.stopMongod(node);
    });

    test.stop();
    jsTestLog("canStartShardSplitWithAbortedMigration test completed");
}

canStartShardSplitWithAbortedMigration({protocol: "multitenant migrations", runOnRecipient: false});
canStartShardSplitWithAbortedMigration({protocol: "shard merge", runOnRecipient: false});
