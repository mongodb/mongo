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

function retrySplit({protocol, recipientTagName, recipientSetName, tenantIds, test, splitRst}) {
    const tenantMigrationId = UUID();
    const firstSplitMigrationId = UUID();
    const secondSplitMigrationId = UUID();

    let recipientNodes = addRecipientNodes({rst: splitRst, recipientTagName});

    let fp = configureFailPoint(test.getDonorRst().getPrimary(),
                                "pauseTenantMigrationBeforeLeavingDataSyncState");

    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(tenantMigrationId),
        protocol,
    };
    if (protocol != "shard merge") {
        migrationOpts["tenantId"] = tenantIds[0];
    }
    jsTestLog("Starting tenant migration");
    assert.commandWorked(test.startMigration(migrationOpts));

    fp.wait();

    const commitThread = commitSplitAsync({
        rst: splitRst,
        tenantIds,
        recipientTagName,
        recipientSetName,
        migrationId: firstSplitMigrationId
    });
    assert.commandFailed(commitThread.returnData());

    fp.off();

    TenantMigrationTest.assertCommitted(test.waitForMigrationToComplete(migrationOpts));
    assert.commandWorked(test.forgetMigration(migrationOpts.migrationIdString));

    // Potential race condition as we do not know how quickly the future continuation in
    // PrimaryOnlyService will remove the instance from its map.
    sleep(1000);
    const secondCommitThread = commitSplitAsync({
        rst: splitRst,
        tenantIds,
        recipientTagName,
        recipientSetName,
        migrationId: secondSplitMigrationId
    });
    assert.commandWorked(secondCommitThread.returnData());

    splitRst.nodes = splitRst.nodes.filter(node => !recipientNodes.includes(node));
    splitRst.ports =
        splitRst.ports.filter(port => !recipientNodes.some(node => node.port === port));

    assert.commandWorked(splitRst.getPrimary().getDB("admin").runCommand(
        {forgetShardSplit: 1, migrationId: secondSplitMigrationId}));

    recipientNodes.forEach(node => {
        MongoRunner.stopMongod(node);
    });
}

// Test that we cannot start a shard split while a migration is in progress.
const recipientTagName = "recipientTag";
const recipientSetName = "recipient";
const tenantIds = ["tenant1", "tenant2"];

sharedOptions = {
    setParameter: {
        shardSplitGarbageCollectionDelayMS: 0,
        tenantMigrationGarbageCollectionDelayMS: 0,
        ttlMonitorSleepSecs: 1
    }
};

const test = new TenantMigrationTest({quickGarbageCollection: true, sharedOptions});

// "multitenant migration" with shard split on donor
retrySplit({
    protocol: "multitenant migrations",
    recipientTagName,
    recipientSetName,
    tenantIds,
    test,
    splitRst: test.getDonorRst()
});

// "multitenant migration" with shard split on recipient
retrySplit({
    protocol: "multitenant migrations",
    recipientTagName,
    recipientSetName,
    tenantIds,
    test,
    splitRst: test.getRecipientRst()
});

// "shard merge" with shard split on donor
retrySplit({
    protocol: "shard merge",
    recipientTagName,
    recipientSetName,
    tenantIds,
    test,
    splitRst: test.getDonorRst()
});

test.stop();

// We need a new test for the next shard merge as adding nodes will cause a crash.
const test2 = new TenantMigrationTest({quickGarbageCollection: true, sharedOptions});

// "shard merge" with shard split on recipient
retrySplit({
    protocol: "multitenant migrations",
    recipientTagName,
    recipientSetName,
    tenantIds,
    test: test2,
    splitRst: test2.getDonorRst()
});

test2.stop();
