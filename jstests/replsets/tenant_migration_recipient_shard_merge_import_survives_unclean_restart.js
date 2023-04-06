/**
 * Tests that the donor data copied via shard merge protocol by recipient is present even after
 * unclean restarts.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 *   featureFlagShardMerge,
 * ]
 */

import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";
import {
    isShardMergeEnabled,
    makeX509OptionsForTest
} from "jstests/replsets/libs/tenant_migration_util.js";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");

// Setting syncdelay to 0 will disable the checkpoint on the recipient.
const recipientRst = new ReplSetTest({
    nodes: 1,
    name: "recipient",
    serverless: true,
    nodeOptions: Object.assign(makeX509OptionsForTest().recipient, {
        setParameter:
            {syncdelay: 0, tenantMigrationGarbageCollectionDelayMS: 0, ttlMonitorSleepSecs: 1}
    })
});

recipientRst.startSet();
recipientRst.initiate();

const recipientPrimary = recipientRst.getPrimary();

// Note: Including this explicit early return here due to the fact that multiversion
// suites will execute this test without featureFlagShardMerge enabled (despite the
// presence of the featureFlagShardMerge tag above), which means the test will attempt
// to run a multi-tenant migration and fail.
if (!isShardMergeEnabled(recipientPrimary.getDB("admin"))) {
    recipientRst.stop();
    jsTestLog("Skipping Shard Merge-specific test.");
    quit();
}

const tenantMigrationTest = new TenantMigrationTest(
    {name: jsTestName(), sharedOptions: {nodes: 1}, recipientRst: recipientRst});
const donorPrimary = tenantMigrationTest.getDonorPrimary();

const tenantId = ObjectId();
const tenantDB = tenantMigrationTest.tenantDB(tenantId.str, "DB");
const collName = "testColl";

// Do a majority write.
tenantMigrationTest.insertDonorDB(tenantDB, collName);

const migrationUuid = UUID();
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(migrationUuid),
    readPreference: {mode: 'primary'},
    tenantIds: [tenantId],
};

// Start migration, and then wait for the migration to get committed and garbage collected.
assert.commandWorked(
    tenantMigrationTest.runMigration(migrationOpts, {automaticForgetMigration: true}));

jsTestLog("Restart recipient primary.");
// Do an unclean shutdown of the recipient primary, and then restart.
recipientRst.restart(recipientPrimary, {allowedExitCode: MongoRunner.EXIT_SIGKILL}, 9);
// Wait for the recipient primary to get elected.
recipientRst.getPrimary();

// Verify the imported donor data is still present on recipient even though the "Checkpointer"
// thread didn’t take a checkpoint before node unclean restart.
recipientRst.nodes.forEach(node => {
    jsTestLog(`Checking ${tenantDB}.${collName} on ${node}`);
    // Use "countDocuments" to check actual docs, "count" to check sizeStorer data.
    assert.eq(donorPrimary.getDB(tenantDB)[collName].countDocuments({}),
              node.getDB(tenantDB)[collName].countDocuments({}),
              "countDocuments");
    assert.eq(donorPrimary.getDB(tenantDB)[collName].count(),
              node.getDB(tenantDB)[collName].count(),
              "count");
});

tenantMigrationTest.stop();
recipientRst.stopSet();
