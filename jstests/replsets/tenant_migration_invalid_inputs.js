/**
 * Tests that the donorStartMigration and recipientSyncData commands throw an error if the provided
 * tenantId is unsupported (i.e. '', 'admin', 'local' or 'config') or if the recipient
 * connection string matches the donor's connection string or doesn't correspond to a replica set
 * with a least one host.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_persistence,
 *   requires_fcv_51,
 *   serverless,
 * ]
 */

(function() {
"use strict";

load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");

const tenantMigrationTest =
    new TenantMigrationTest({name: jsTestName(), enableRecipientTesting: false});

const donorPrimary = tenantMigrationTest.getDonorPrimary();
const recipientPrimary = tenantMigrationTest.getRecipientPrimary();

const tenantId = "testTenantId";
const readPreference = {
    mode: 'primary'
};
const migrationCertificates = TenantMigrationUtil.makeMigrationCertificatesForTest();

jsTestLog("Testing 'donorStartMigration' command provided with invalid options.");

// Test missing tenantId field for protocol 'multitenant migrations'.
assert.commandFailedWithCode(
    donorPrimary.adminCommand(
        TenantMigrationUtil.donorStartMigrationWithProtocol({
            donorStartMigration: 1,
            protocol: 'multitenant migrations',
            migrationId: UUID(),
            recipientConnectionString: tenantMigrationTest.getRecipientRst().getURL(),
            readPreference: readPreference,
            donorCertificateForRecipient: migrationCertificates.donorCertificateForRecipient,
            recipientCertificateForDonor: migrationCertificates.recipientCertificateForDonor,
        },
                                                            donorPrimary.getDB("admin"))),
    ErrorCodes.InvalidOptions);

// Test unsupported database prefixes.
const unsupportedtenantIds = ['', 'admin', 'local', 'config'];
unsupportedtenantIds.forEach((invalidTenantId) => {
    assert.commandFailedWithCode(
        donorPrimary.adminCommand(
            TenantMigrationUtil.donorStartMigrationWithProtocol({
                donorStartMigration: 1,
                migrationId: UUID(),
                recipientConnectionString: tenantMigrationTest.getRecipientRst().getURL(),
                tenantId: invalidTenantId,
                readPreference: readPreference,
                donorCertificateForRecipient: migrationCertificates.donorCertificateForRecipient,
                recipientCertificateForDonor: migrationCertificates.recipientCertificateForDonor,
            },
                                                                donorPrimary.getDB("admin"))),
        ErrorCodes.BadValue);
});

// Test migrating a tenant to the donor itself.
assert.commandFailedWithCode(
    donorPrimary.adminCommand(
        TenantMigrationUtil.donorStartMigrationWithProtocol({
            donorStartMigration: 1,
            migrationId: UUID(),
            recipientConnectionString: tenantMigrationTest.getDonorRst().getURL(),
            tenantId: tenantId,
            readPreference: readPreference,
            donorCertificateForRecipient: migrationCertificates.donorCertificateForRecipient,
            recipientCertificateForDonor: migrationCertificates.recipientCertificateForDonor,
        },
                                                            donorPrimary.getDB("admin"))),
    ErrorCodes.BadValue);

// Test migrating a tenant to a recipient that shares one or more hosts with the donor.
assert.commandFailedWithCode(
    donorPrimary.adminCommand(
        TenantMigrationUtil.donorStartMigrationWithProtocol({
            donorStartMigration: 1,
            migrationId: UUID(),
            recipientConnectionString:
                tenantMigrationTest.getRecipientRst().getURL() + "," + donorPrimary.host,
            tenantId: tenantId,
            readPreference: readPreference,
            donorCertificateForRecipient: migrationCertificates.donorCertificateForRecipient,
            recipientCertificateForDonor: migrationCertificates.recipientCertificateForDonor,
        },
                                                            donorPrimary.getDB("admin"))),
    ErrorCodes.BadValue);

// Test migrating a tenant to a standalone recipient.
assert.commandFailedWithCode(
    donorPrimary.adminCommand(
        TenantMigrationUtil.donorStartMigrationWithProtocol({
            donorStartMigration: 1,
            migrationId: UUID(),
            recipientConnectionString: recipientPrimary.host,
            tenantId: tenantId,
            readPreference: readPreference,
            donorCertificateForRecipient: migrationCertificates.donorCertificateForRecipient,
            recipientCertificateForDonor: migrationCertificates.recipientCertificateForDonor,
        },
                                                            donorPrimary.getDB("admin"))),
    ErrorCodes.BadValue);

jsTestLog("Testing 'recipientSyncData' command provided with invalid options.");

// Test missing tenantId field for protocol 'multitenant migrations'.
assert.commandFailedWithCode(recipientPrimary.adminCommand({
    recipientSyncData: 1,
    protocol: 'multitenant migrations',
    migrationId: UUID(),
    donorConnectionString: tenantMigrationTest.getDonorRst().getURL(),
    startMigrationDonorTimestamp: Timestamp(1, 1),
    readPreference: readPreference,
    recipientCertificateForDonor: migrationCertificates.recipientCertificateForDonor,
}),
                             ErrorCodes.InvalidOptions);

// Test unsupported database prefixes.
unsupportedtenantIds.forEach((invalidTenantId) => {
    assert.commandFailedWithCode(recipientPrimary.adminCommand({
        recipientSyncData: 1,
        migrationId: UUID(),
        donorConnectionString: tenantMigrationTest.getDonorRst().getURL(),
        tenantId: invalidTenantId,
        startMigrationDonorTimestamp: Timestamp(1, 1),
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
    startMigrationDonorTimestamp: Timestamp(1, 1),
    readPreference: readPreference,
    recipientCertificateForDonor: migrationCertificates.recipientCertificateForDonor,
}),
                             ErrorCodes.BadValue);

// Test migrating a tenant from a donor that shares one or more hosts with the recipient.
assert.commandFailedWithCode(recipientPrimary.adminCommand({
    recipientSyncData: 1,
    migrationId: UUID(),
    donorConnectionString: tenantMigrationTest.getDonorRst().getURL() + "," + recipientPrimary.host,
    tenantId: tenantId,
    startMigrationDonorTimestamp: Timestamp(1, 1),
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
    startMigrationDonorTimestamp: Timestamp(1, 1),
    readPreference: readPreference,
    recipientCertificateForDonor: migrationCertificates.recipientCertificateForDonor,
}),
                             ErrorCodes.BadValue);

// Test 'returnAfterReachingDonorTimestamp' can't be null.
const nullTimestamps = [Timestamp(0, 0), Timestamp(0, 1)];
nullTimestamps.forEach((nullTs) => {
    assert.commandFailedWithCode(donorPrimary.adminCommand({
        recipientSyncData: 1,
        migrationId: UUID(),
        donorConnectionString: tenantMigrationTest.getDonorRst().getURL(),
        tenantId: tenantId,
        startMigrationDonorTimestamp: Timestamp(1, 1),
        readPreference: readPreference,
        returnAfterReachingDonorTimestamp: nullTs,
        recipientCertificateForDonor: migrationCertificates.recipientCertificateForDonor,
    }),
                                 ErrorCodes.BadValue);
});

tenantMigrationTest.stop();
})();
