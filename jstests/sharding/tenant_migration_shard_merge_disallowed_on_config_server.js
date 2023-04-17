/**
 * Tests that tenant migration commands cannot be run on sharded clusters for config servers for
 * shard merge protocol.
 *
 * @tags: [
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   does_not_support_stepdowns,
 *   featureFlagShardMerge,
 *   serverless,
 * ]
 */

import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";
import {isShardMergeEnabled} from "jstests/replsets/libs/tenant_migration_util.js";

(function() {
load("jstests/libs/config_shard_util.js");

const st = new ShardingTest({shards: 1});
const donorRstShard = st.rs0;
const donorRstConfig = st.configRS;

// Note: including this explicit early return here due to the fact that multiversion
// suites will execute this test without featureFlagShardMerge enabled (despite the
// presence of the featureFlagShardMerge tag above), which means the test will attempt
// to run a multi-tenant migration and fail.
if (!isShardMergeEnabled(donorRstShard.getPrimary().getDB("admin"))) {
    jsTestLog("Skipping Shard Merge-specific test");
    st.stop();
    quit();
}

const recipientRst = new ReplSetTest({nodes: 1});
recipientRst.startSet();
recipientRst.initiate();

const tenantMigrationTest =
    new TenantMigrationTest({name: jsTestName(), donorRst: donorRstShard, recipientRst});

// Run tenant migration commands on config servers.
let donorPrimary = donorRstConfig.getPrimary();

let cmdObj = {
    donorStartMigration: 1,
    tenantIds: [ObjectId()],
    migrationId: UUID(),
    protocol: "shard merge",
    recipientConnectionString: tenantMigrationTest.getRecipientConnString(),
    readPreference: {mode: "primary"}
};
assert.commandFailedWithCode(donorPrimary.adminCommand(cmdObj), ErrorCodes.IllegalOperation);

cmdObj = {
    donorForgetMigration: 1,
    migrationId: UUID()
};
assert.commandFailedWithCode(donorPrimary.adminCommand(cmdObj), ErrorCodes.IllegalOperation);

cmdObj = {
    donorAbortMigration: 1,
    migrationId: UUID()
};
assert.commandFailedWithCode(donorPrimary.adminCommand(cmdObj), ErrorCodes.IllegalOperation);

cmdObj = {
    recipientSyncData: 1,
    migrationId: UUID(),
    donorConnectionString: tenantMigrationTest.getRecipientRst().getURL(),
    tenantIds: [ObjectId()],
    protocol: "shard merge",
    readPreference: {mode: "primary"},
    startMigrationDonorTimestamp: Timestamp(1, 1)
};
assert.commandFailedWithCode(donorPrimary.adminCommand(cmdObj), ErrorCodes.IllegalOperation);

cmdObj = {
    recipientForgetMigration: 1,
    migrationId: UUID(),
    donorConnectionString: tenantMigrationTest.getRecipientRst().getURL(),
    tenantIds: [ObjectId()],
    protocol: "shard merge",
    readPreference: {mode: "primary"},
};
assert.commandFailedWithCode(donorPrimary.adminCommand(cmdObj), ErrorCodes.IllegalOperation);

tenantMigrationTest.stop();
recipientRst.stopSet();
st.stop();
})();
