/**
 * Test that tenant migration commands do not require ssl
 * when 'tenantMigrationDisableX509Auth' server parameter is true (default).
 *
 * @tags: [
 *   incompatible_with_macos,
 *   # Shard merge protocol will be tested by tenant_migration_shard_merge_ssl_configuration.js.
 *   incompatible_with_shard_merge,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 *   requires_fcv_71,
 * ]
 */

import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";
import {isShardMergeEnabled} from "jstests/replsets/libs/tenant_migration_util.js";

const kTenantId = ObjectId().str;
const kReadPreference = {
    mode: "primary"
};

(() => {
    const donorRst = new ReplSetTest({
        nodes: 1,
        name: "donor",
        serverless: true,
        nodeOptions: {setParameter: {tenantMigrationDisableX509Auth: false}}
    });

    const recipientRst = new ReplSetTest({
        nodes: 1,
        name: "recipient",
        serverless: true,
        nodeOptions: {setParameter: {tenantMigrationDisableX509Auth: false}}
    });

    donorRst.startSet();
    donorRst.initiate();

    if (isShardMergeEnabled(donorRst.getPrimary().getDB("admin"))) {
        donorRst.stopSet();
        jsTestLog("Skipping this shard merge incompatible test.");
        quit();
    }

    recipientRst.startSet();
    recipientRst.initiate();

    const tenantMigrationTest =
        new TenantMigrationTest({name: jsTestName(), donorRst, recipientRst});
    const donorPrimary = tenantMigrationTest.getDonorPrimary();
    const recipientPrimary = tenantMigrationTest.getRecipientPrimary();

    jsTest.log("Test that recipientSyncData requires tenantMigrationDisableX509Auth=true");
    assert.commandFailedWithCode(recipientPrimary.adminCommand({
        recipientSyncData: 1,
        migrationId: UUID(),
        donorConnectionString: tenantMigrationTest.getDonorRst().getURL(),
        tenantId: kTenantId,
        startMigrationDonorTimestamp: Timestamp(1, 1),
        readPreference: kReadPreference
    }),
                                 ErrorCodes.IllegalOperation);

    jsTest.log("Test that recipientForgetMigration requires tenantMigrationDisableX509Auth=true");
    assert.commandFailedWithCode(recipientPrimary.adminCommand({
        recipientForgetMigration: 1,
        migrationId: UUID(),
        donorConnectionString: tenantMigrationTest.getDonorRst().getURL(),
        tenantId: kTenantId,
        readPreference: kReadPreference
    }),
                                 ErrorCodes.IllegalOperation);

    jsTest.log("Test that donorStartMigration requires tenantMigrationDisableX509Auth=true");
    assert.commandFailedWithCode(donorPrimary.adminCommand({
        donorStartMigration: 1,
        migrationId: UUID(),
        recipientConnectionString: tenantMigrationTest.getRecipientRst().getURL(),
        tenantId: kTenantId,
        readPreference: kReadPreference,
    }),
                                 ErrorCodes.IllegalOperation);

    recipientRst.stopSet();
    donorRst.stopSet();
    tenantMigrationTest.stop();
})();

(() => {
    jsTest.log("Test that tenant migration doesn't fail if SSL is not enabled on the donor and " +
               "the recipient and tenantMigrationDisableX509Auth=true");

    const donorRst = new ReplSetTest({
        nodes: 1,
        name: "donor",
        serverless: true,
        nodeOptions: {setParameter: {tenantMigrationDisableX509Auth: true}}
    });
    const recipientRst = new ReplSetTest({
        nodes: 1,
        name: "recipient",
        serverless: true,
        nodeOptions: {setParameter: {tenantMigrationDisableX509Auth: true}}
    });

    donorRst.startSet();
    donorRst.initiate();

    recipientRst.startSet();
    recipientRst.initiate();

    const tenantMigrationTest =
        new TenantMigrationTest({name: jsTestName(), donorRst, recipientRst});

    const donorStartMigrationCmdObj = {
        donorStartMigration: 1,
        migrationIdString: extractUUIDFromObject(UUID()),
        recipientConnectionString: tenantMigrationTest.getRecipientRst().getURL(),
        tenantId: kTenantId,
        readPreference: kReadPreference
    };

    const stateRes =
        assert.commandWorked(tenantMigrationTest.runMigration(donorStartMigrationCmdObj));
    assert.eq(stateRes.state, TenantMigrationTest.DonorState.kCommitted);

    recipientRst.stopSet();
    donorRst.stopSet();
    tenantMigrationTest.stop();
})();
