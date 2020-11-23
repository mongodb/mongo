/**
 * Tests that the donorStartMigration and recipientSyncData commands throw an error if the provided
 * tenantId is unsupported (i.e. '', 'admin', 'local' or 'config') or if the recipient
 * connection string matches the donor's connection string or doesn't correspond to a replica set
 * with a least one host.
 *
 * @tags: [requires_fcv_47]
 */

(function() {
"use strict";

load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");

const tenantMigrationTest =
    new TenantMigrationTest({name: jsTestName(), enableRecipientTesting: false});

if (!tenantMigrationTest.isFeatureFlagEnabled()) {
    jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
    return;
}

const donorPrimary = tenantMigrationTest.getDonorPrimary();
const recipientPrimary = tenantMigrationTest.getRecipientPrimary();

const tenantId = "testTenantId";
const readPreference = {
    mode: 'primary'
};
const migrationCertificates = TenantMigrationUtil.makeMigrationCertificatesForTest();

jsTestLog("Testing 'donorStartMigration' command provided with invalid options.");

// Test unsupported database prefixes.
const unsupportedtenantIds = ['', 'admin', 'local', 'config'];
unsupportedtenantIds.forEach((invalidTenantId) => {
    assert.commandFailedWithCode(donorPrimary.adminCommand({
        donorStartMigration: 1,
        migrationId: UUID(),
        recipientConnectionString: tenantMigrationTest.getRecipientRst().getURL(),
        tenantId: invalidTenantId,
        readPreference: readPreference,
        donorCertificateForRecipient: migrationCertificates.donorCertificateForRecipient,
        recipientCertificateForDonor: migrationCertificates.recipientCertificateForDonor,
    }),
                                 ErrorCodes.BadValue);
});

// Test migrating a tenant to the donor itself.
assert.commandFailedWithCode(donorPrimary.adminCommand({
    donorStartMigration: 1,
    migrationId: UUID(),
    recipientConnectionString: tenantMigrationTest.getDonorRst().getURL(),
    tenantId: tenantId,
    readPreference: readPreference,
    donorCertificateForRecipient: migrationCertificates.donorCertificateForRecipient,
    recipientCertificateForDonor: migrationCertificates.recipientCertificateForDonor,
}),
                             ErrorCodes.BadValue);

// Test migrating a tenant to a recipient that shares one or more hosts with the donor.
assert.commandFailedWithCode(donorPrimary.adminCommand({
    donorStartMigration: 1,
    migrationId: UUID(),
    recipientConnectionString:
        tenantMigrationTest.getRecipientRst().getURL() + "," + donorPrimary.host,
    tenantId: tenantId,
    readPreference: readPreference
}),
                             ErrorCodes.BadValue);

// Test migrating a tenant to a standalone recipient.
assert.commandFailedWithCode(donorPrimary.adminCommand({
    donorStartMigration: 1,
    migrationId: UUID(),
    recipientConnectionString: recipientPrimary.host,
    tenantId: tenantId,
    readPreference: readPreference,
    donorCertificateForRecipient: migrationCertificates.donorCertificateForRecipient,
    recipientCertificateForDonor: migrationCertificates.recipientCertificateForDonor,
}),
                             ErrorCodes.BadValue);

jsTestLog("Testing 'recipientSyncData' command provided with invalid options.");

// Test unsupported database prefixes.
unsupportedtenantIds.forEach((invalidTenantId) => {
    assert.commandFailedWithCode(recipientPrimary.adminCommand({
        recipientSyncData: 1,
        migrationId: UUID(),
        donorConnectionString: tenantMigrationTest.getDonorRst().getURL(),
        tenantId: invalidTenantId,
        readPreference: readPreference,
        recipientCertificateForDonor: migrationCertificates.recipientCertificateForDonor,
    }),
                                 ErrorCodes.BadValue);
});

// Test migrating a tenant from the recipient itself.
assert.commandFailedWithCode(recipientPrimary.adminCommand({
    recipientSyncData: 1,
    migrationId: UUID(),
    donorConnectionString: tenantMigrationTest.getRecipientRst().getURL(),
    tenantId: tenantId,
    readPreference: readPreference
}),
                             ErrorCodes.BadValue);

// Test migrating a tenant from a donor that shares one or more hosts with the recipient.
assert.commandFailedWithCode(recipientPrimary.adminCommand({
    recipientSyncData: 1,
    migrationId: UUID(),
    donorConnectionString: tenantMigrationTest.getDonorRst().getURL() + "," + recipientPrimary.host,
    tenantId: tenantId,
    readPreference: readPreference,
    recipientCertificateForDonor: migrationCertificates.recipientCertificateForDonor,
}),
                             ErrorCodes.BadValue);

// Test migrating a tenant from a standalone donor.
assert.commandFailedWithCode(recipientPrimary.adminCommand({
    recipientSyncData: 1,
    migrationId: UUID(),
    donorConnectionString: recipientPrimary.host,
    tenantId: tenantId,
    readPreference: readPreference
}),
                             ErrorCodes.BadValue);

// Test 'returnAfterReachingDonorTimestamp' can' be null.
const nullTimestamps = [Timestamp(0, 0), Timestamp(0, 1)];
nullTimestamps.forEach((nullTs) => {
    assert.commandFailedWithCode(donorPrimary.adminCommand({
        recipientSyncData: 1,
        migrationId: UUID(),
        donorConnectionString: tenantMigrationTest.getDonorRst().getURL(),
        tenantId: tenantId,
        readPreference: readPreference,
        returnAfterReachingDonorTimestamp: nullTs
    }),
                                 ErrorCodes.BadValue);
});

tenantMigrationTest.stop();
})();
