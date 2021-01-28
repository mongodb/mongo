/**
 * Test that tenant migration commands require certificate fields and SSL to be enabled.
 *
 * @tags: [requires_fcv_47, requires_majority_read_concern, incompatible_with_eft]
 */

(function() {
"use strict";

load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");

const kTenantId = "testTenantId";
const kReadPreference = {
    mode: "primary"
};
const kMigrationCertificates = TenantMigrationUtil.makeMigrationCertificatesForTest();

(() => {
    jsTest.log("Test that certificate fields are required fields for donorStartMigration and " +
               "recipientSyncData");
    const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});
    if (!tenantMigrationTest.isFeatureFlagEnabled()) {
        jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
        return;
    }

    const donorPrimary = tenantMigrationTest.getDonorPrimary();
    const recipientPrimary = tenantMigrationTest.getRecipientPrimary();

    jsTest.log(
        "Test that 'donorCertificateForRecipient' is a required field for donorStartMigration");
    assert.commandFailedWithCode(donorPrimary.adminCommand({
        donorStartMigration: 1,
        migrationId: UUID(),
        recipientConnectionString: tenantMigrationTest.getRecipientRst().getURL(),
        tenantId: kTenantId,
        readPreference: kReadPreference,
        recipientCertificateForDonor: kMigrationCertificates.recipientCertificateForDonor,
    }),
                                 40414);

    jsTest.log(
        "Test that 'recipientCertificateForDonor' is a required field for donorStartMigration");
    assert.commandFailedWithCode(donorPrimary.adminCommand({
        donorStartMigration: 1,
        migrationId: UUID(),
        recipientConnectionString: tenantMigrationTest.getRecipientRst().getURL(),
        tenantId: kTenantId,
        readPreference: kReadPreference,
        donorCertificateForRecipient: kMigrationCertificates.donorCertificateForRecipient,
    }),
                                 40414);

    jsTest.log(
        "Test that 'recipientCertificateForDonor' is a required field for recipientSyncData");
    assert.commandFailedWithCode(recipientPrimary.adminCommand({
        recipientSyncData: 1,
        migrationId: UUID(),
        donorConnectionString: tenantMigrationTest.getDonorRst().getURL(),
        tenantId: kTenantId,
        readPreference: kReadPreference
    }),
                                 40414);

    jsTest.log(
        "Test that 'recipientCertificateForDonor' is a required field for recipientForgetMigration");
    assert.commandFailedWithCode(recipientPrimary.adminCommand({
        recipientForgetMigration: 1,
        migrationId: UUID(),
        donorConnectionString: tenantMigrationTest.getDonorRst().getURL(),
        tenantId: kTenantId,
        readPreference: kReadPreference
    }),
                                 40414);

    tenantMigrationTest.stop();
})();

(() => {
    jsTest.log("Test that donorStartMigration fails if SSL is not enabled on the donor");
    const donorRst = new ReplSetTest({nodes: 1, name: "donor"});
    donorRst.startSet();
    donorRst.initiate();

    const tenantMigrationTest = new TenantMigrationTest({name: jsTestName(), donorRst});
    if (!tenantMigrationTest.isFeatureFlagEnabled()) {
        jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
        donorRst.stopSet();
        return;
    }

    const donorPrimary = tenantMigrationTest.getDonorPrimary();

    assert.commandFailedWithCode(donorPrimary.adminCommand({
        donorStartMigration: 1,
        migrationId: UUID(),
        recipientConnectionString: tenantMigrationTest.getRecipientRst().getURL(),
        tenantId: kTenantId,
        readPreference: kReadPreference,
        donorCertificateForRecipient: kMigrationCertificates.donorCertificateForRecipient,
        recipientCertificateForDonor: kMigrationCertificates.recipientCertificateForDonor,
    }),
                                 ErrorCodes.IllegalOperation);

    donorRst.stopSet();
    tenantMigrationTest.stop();
})();

(() => {
    jsTest.log("Test that recipientSyncData fails if SSL is not enabled on the recipient");
    const recipientRst = new ReplSetTest({nodes: 1, name: "recipient"});
    recipientRst.startSet();
    recipientRst.initiate();

    const tenantMigrationTest = new TenantMigrationTest({name: jsTestName(), recipientRst});
    if (!tenantMigrationTest.isFeatureFlagEnabled()) {
        jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
        recipientRst.stopSet();
        return;
    }

    const recipientPrimary = tenantMigrationTest.getRecipientPrimary();

    assert.commandFailedWithCode(recipientPrimary.adminCommand({
        recipientSyncData: 1,
        migrationId: UUID(),
        donorConnectionString: tenantMigrationTest.getDonorRst().getURL(),
        tenantId: kTenantId,
        readPreference: kReadPreference,
        recipientCertificateForDonor: kMigrationCertificates.recipientCertificateForDonor,
    }),
                                 ErrorCodes.IllegalOperation);

    recipientRst.stopSet();
    tenantMigrationTest.stop();
})();

(() => {
    jsTest.log("Test that donorStartMigration fails if SSL is not enabled on the recipient");
    const recipientRst = new ReplSetTest({nodes: 1, name: "recipient"});
    recipientRst.startSet();
    recipientRst.initiate();

    const tenantMigrationTest = new TenantMigrationTest({name: jsTestName(), recipientRst});
    if (!tenantMigrationTest.isFeatureFlagEnabled()) {
        jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
        recipientRst.stopSet();
        return;
    }

    const donorRst = tenantMigrationTest.getDonorRst();

    const donorStartMigrationCmdObj = {
        donorStartMigration: 1,
        migrationId: UUID(),
        recipientConnectionString: tenantMigrationTest.getRecipientRst().getURL(),
        tenantId: kTenantId,
        readPreference: kReadPreference,
        donorCertificateForRecipient: kMigrationCertificates.donorCertificateForRecipient,
        recipientCertificateForDonor: kMigrationCertificates.recipientCertificateForDonor,
    };

    const stateRes = assert.commandWorked(TenantMigrationUtil.runTenantMigrationCommand(
        donorStartMigrationCmdObj,
        donorRst,
        false /* retryOnRetryableErrors */,
        TenantMigrationUtil.isMigrationCompleted /* shouldStopFunc */));
    assert.eq(stateRes.state, TenantMigrationTest.State.kAborted);
    // The command should fail with HostUnreachable since the donor was unable to establish an SSL
    // connection with the recipient.
    assert.eq(stateRes.abortReason.code, ErrorCodes.HostUnreachable);

    recipientRst.stopSet();
    tenantMigrationTest.stop();
})();
})();
