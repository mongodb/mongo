/**
 * Tests that recipient is able to copy and apply cluster parameters from the donor for the shard
 * merge protocol.
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
import {isShardMergeEnabled} from "jstests/replsets/libs/tenant_migration_util.js";

load("jstests/libs/cluster_server_parameter_utils.js");
load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");

const tenantMigrationTest = new TenantMigrationTest({
    name: jsTestName(),
    quickGarbageCollection: true,
    sharedOptions: {
        setParameter: {
            multitenancySupport: true,
        },
    },
});

// Note: including this explicit early return here due to the fact that multiversion
// suites will execute this test without featureFlagShardMerge enabled (despite the
// presence of the featureFlagShardMerge tag above), which means the test will attempt
// to run a multi-tenant migration and fail.
if (!isShardMergeEnabled(tenantMigrationTest.getRecipientPrimary().getDB("admin"))) {
    tenantMigrationTest.stop();
    jsTestLog("Skipping Shard Merge-specific test");
    quit();
}

const donorPrimary = tenantMigrationTest.getDonorPrimary();
const recipientPrimary = tenantMigrationTest.getRecipientPrimary();

const tenantId = ObjectId();

// Set a cluster parameter before the migration starts.
assert.commandWorked(donorPrimary.getDB("admin").runCommand(tenantCommand(
    {setClusterParameter: {"changeStreams": {"expireAfterSeconds": 7200}}}, tenantId)));

const fpBeforeMarkingCloneSuccess =
    configureFailPoint(recipientPrimary, "fpBeforeMarkingCloneSuccess", {action: "hang"});

const migrationUuid = UUID();
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(migrationUuid),
    readPreference: {mode: "primary"},
    tenantIds: [tenantId],
};

assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));

fpBeforeMarkingCloneSuccess.wait();

// Set another cluster parameter so that oplog entries are applied during oplog catchup.
assert.commandWorked(donorPrimary.getDB("admin").runCommand(tenantCommand(
    {setClusterParameter: {"testStrClusterParameter": {"strData": "sleep"}}}, tenantId)));

fpBeforeMarkingCloneSuccess.off();

TenantMigrationTest.assertCommitted(tenantMigrationTest.waitForMigrationToComplete(migrationOpts));

const {clusterParameters} = assert.commandWorked(recipientPrimary.getDB("admin").runCommand(
    tenantCommand({getClusterParameter: ["changeStreams", "testStrClusterParameter"]}, tenantId)));
const [changeStreamsClusterParameter, testStrClusterParameter] = clusterParameters;
assert.eq(changeStreamsClusterParameter.expireAfterSeconds, 7200);
assert.eq(testStrClusterParameter.strData, "sleep");

assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
tenantMigrationTest.waitForMigrationGarbageCollection(migrationOpts.migrationIdString);

tenantMigrationTest.stop();
