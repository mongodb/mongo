/**
 * Test that shard merge commands do not require ssl
 * when 'tenantMigrationDisableX509Auth' server parameter is true (default).
 *
 * @tags: [
 *   incompatible_with_macos,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   requires_shard_merge,
 *   serverless,
 *   requires_fcv_71,
 * ]
 */

import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";
import {
    kProtocolShardMerge,
} from "jstests/replsets/libs/tenant_migration_util.js";

const kTenantId = ObjectId();
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
        tenantIds: [kTenantId],
        protocol: kProtocolShardMerge,
        startMigrationDonorTimestamp: Timestamp(1, 1),
        readPreference: kReadPreference
    }),
                                 ErrorCodes.IllegalOperation);

    jsTest.log("Test that recipientForgetMigration requires tenantMigrationDisableX509Auth=true");
    assert.commandFailedWithCode(recipientPrimary.adminCommand({
        recipientForgetMigration: 1,
        migrationId: UUID(),
        donorConnectionString: tenantMigrationTest.getDonorRst().getURL(),
        tenantIds: [kTenantId],
        protocol: kProtocolShardMerge,
        readPreference: kReadPreference
    }),
                                 ErrorCodes.IllegalOperation);

    jsTest.log("Test that donorStartMigration requires tenantMigrationDisableX509Auth=true");
    assert.commandFailedWithCode(donorPrimary.adminCommand({
        donorStartMigration: 1,
        migrationId: UUID(),
        recipientConnectionString: tenantMigrationTest.getRecipientRst().getURL(),
        tenantIds: [kTenantId],
        protocol: kProtocolShardMerge,
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
        tenantIds: [kTenantId],
        protocol: kProtocolShardMerge,
        readPreference: kReadPreference
    };

    const stateRes =
        assert.commandWorked(tenantMigrationTest.runMigration(donorStartMigrationCmdObj));
    assert.eq(stateRes.state, TenantMigrationTest.DonorState.kCommitted);

    donorRst.stopSet();
    recipientRst.stopSet();
    tenantMigrationTest.stop();
})();
