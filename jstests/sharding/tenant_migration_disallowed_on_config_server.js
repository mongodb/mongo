/**
 * Tests that tenant migration commands cannot be run on sharded clusters for config servers.
 *
 * @tags: [
 *   incompatible_with_windows_tls,
 *   # Shard merge protocol will be tested by
 *   # tenant_migration_shard_merge_disallowed_on_config_server.js.
 *   incompatible_with_shard_merge,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   does_not_support_stepdowns,
 *   serverless,
 * ]
 */

import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";
import {donorStartMigrationWithProtocol} from "jstests/replsets/libs/tenant_migration_util.js";

(function() {
load("jstests/libs/config_shard_util.js");

const st = new ShardingTest({shards: 1});
const donorRstShard = st.rs0;
const donorRstConfig = st.configRS;

const recipientRst = new ReplSetTest({nodes: 1});
recipientRst.startSet();
recipientRst.initiate();

const tenantMigrationTest =
    new TenantMigrationTest({name: jsTestName(), donorRst: donorRstShard, recipientRst});

// Run tenant migration commands on config servers.
let donorPrimary = donorRstConfig.getPrimary();

let cmdObj = donorStartMigrationWithProtocol({
    donorStartMigration: 1,
    tenantId: ObjectId().str,
    migrationId: UUID(),
    recipientConnectionString: tenantMigrationTest.getRecipientConnString(),
    readPreference: {mode: "primary"}
},
                                             donorPrimary.getDB("admin"));
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
    tenantId: ObjectId().str,
    readPreference: {mode: "primary"},
    startMigrationDonorTimestamp: Timestamp(1, 1)
};
assert.commandFailedWithCode(donorPrimary.adminCommand(cmdObj), ErrorCodes.IllegalOperation);

cmdObj = {
    recipientForgetMigration: 1,
    migrationId: UUID(),
    donorConnectionString: tenantMigrationTest.getRecipientRst().getURL(),
    tenantId: ObjectId().str,
    readPreference: {mode: "primary"},
};
assert.commandFailedWithCode(donorPrimary.adminCommand(cmdObj), ErrorCodes.IllegalOperation);

tenantMigrationTest.stop();
recipientRst.stopSet();
st.stop();
})();
