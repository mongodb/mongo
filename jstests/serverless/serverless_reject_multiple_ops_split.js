/**
 * @tags: [
 *   serverless,
 *   requires_fcv_62,
 *   featureFlagShardMerge
 * ]
 */

import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";
import {addRecipientNodes, commitSplitAsync} from "jstests/serverless/libs/shard_split_test.js";

load("jstests/libs/uuid_util.js");

function cannotStartShardSplitWithMigrationInProgress(
    {recipientTagName, protocol, shardSplitRst, test}) {
    // Test that we cannot start a shard split while a migration is in progress.
    const recipientSetName = "recipient";
    const tenantIds = [ObjectId(), ObjectId()];
    const splitMigrationId = UUID();
    const tenantMigrationId = UUID();

    let fp = configureFailPoint(test.getDonorRst().getPrimary(),
                                "pauseTenantMigrationBeforeLeavingDataSyncState");
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(tenantMigrationId),
        protocol,
    };
    if (protocol != "shard merge") {
        migrationOpts["tenantId"] = tenantIds[0].str;
    } else {
        migrationOpts["tenantIds"] = tenantIds;
    }
    jsTestLog("Starting tenant migration");
    assert.commandWorked(test.startMigration(migrationOpts));

    fp.wait();

    const commitThread = commitSplitAsync({
        rst: shardSplitRst,
        tenantIds,
        recipientTagName,
        recipientSetName,
        migrationId: splitMigrationId
    });
    assert.commandFailed(commitThread.returnData());

    fp.off();

    TenantMigrationTest.assertCommitted(test.waitForMigrationToComplete(migrationOpts));
    assert.commandWorked(test.forgetMigration(migrationOpts.migrationIdString));

    jsTestLog("cannotStartShardSplitWithMigrationInProgress test completed");
}

const sharedOptions = {};
sharedOptions["setParameter"] = {
    shardSplitGarbageCollectionDelayMS: 0,
    tenantMigrationGarbageCollectionDelayMS: 0,
    ttlMonitorSleepSecs: 1
};

const recipientTagName = "recipientTag";

const test = new TenantMigrationTest({quickGarbageCollection: true, sharedOptions});
addRecipientNodes({rst: test.getDonorRst(), recipientTagName});
addRecipientNodes({rst: test.getRecipientRst(), recipientTagName});

cannotStartShardSplitWithMigrationInProgress({
    recipientTagName,
    protocol: "multitenant migrations",
    shardSplitRst: test.getDonorRst(),
    test
});
cannotStartShardSplitWithMigrationInProgress(
    {recipientTagName, protocol: "shard merge", shardSplitRst: test.getDonorRst(), test});

cannotStartShardSplitWithMigrationInProgress({
    recipientTagName,
    protocol: "multitenant migrations",
    shardSplitRst: test.getRecipientRst(),
    test
});
cannotStartShardSplitWithMigrationInProgress(
    {recipientTagName, protocol: "shard merge", shardSplitRst: test.getRecipientRst(), test});

test.stop();
