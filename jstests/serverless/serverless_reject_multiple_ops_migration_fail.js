/**
 * @tags: [
 *   serverless,
 *   requires_fcv_62,
 *   featureFlagShardMerge
 * ]
 */

import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";
import {
    addRecipientNodes,
    commitSplitAsync,
    waitForGarbageCollectionForSplit
} from "jstests/serverless/libs/shard_split_test.js";

load("jstests/libs/uuid_util.js");

function cannotStartMigrationWhileShardSplitIsInProgress(protocol) {
    // Test that we cannot start a migration while a shard split is in progress.
    const recipientTagName = "recipientTag";
    const recipientSetName = "recipient";
    const tenantIds = [ObjectId(), ObjectId()];
    const splitMigrationId = UUID();
    const tenantMigrationId = UUID();

    const sharedOptions = {};
    sharedOptions["setParameter"] = {shardSplitGarbageCollectionDelayMS: 0, ttlMonitorSleepSecs: 1};

    const test = new TenantMigrationTest({quickGarbageCollection: true, sharedOptions});

    const splitRst = test.getDonorRst();

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
        migrationOpts["tenantId"] = tenantIds[0].str;
    } else {
        migrationOpts["tenantIds"] = tenantIds;
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
