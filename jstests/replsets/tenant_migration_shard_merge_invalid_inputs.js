/**
 * Tests that the donorStartMigration and recipientSyncData commands for a shard merge throw an
 * error if a tenantId is provided or if the prefix is invalid (i.e. '', 'admin', 'local' or
 * 'config') or if the recipient connection string matches the donor's connection string or doesn't
 * correspond to a replica set with a least one host.
 *
 * @tags: [
 *   incompatible_with_eft,
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   featureFlagShardMerge,
 *   requires_persistence,
 *   serverless,
 * ]
 */

import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";
import {
    isShardMergeEnabled,
    makeMigrationCertificatesForTest,
} from "jstests/replsets/libs/tenant_migration_util.js";

const tenantMigrationTest =
    new TenantMigrationTest({name: jsTestName(), enableRecipientTesting: false});

const donorPrimary = tenantMigrationTest.getDonorPrimary();

// Note: including this explicit early return here due to the fact that multiversion
// suites will execute this test without featureFlagShardMerge enabled (despite the
// presence of the featureFlagShardMerge tag above), which means the test will attempt
// to run a multi-tenant migration and fail.
if (!isShardMergeEnabled(donorPrimary.getDB("admin"))) {
    tenantMigrationTest.stop();
    jsTestLog("Skipping Shard Merge-specific test");
    quit();
}

const recipientPrimary = tenantMigrationTest.getRecipientPrimary();

const tenantId = "testTenantId";
const readPreference = {
    mode: 'primary'
};
const migrationCertificates = makeMigrationCertificatesForTest();

jsTestLog("Testing 'donorStartMigration' command provided with invalid options.");

// Test erroneously included tenantId field and unsupported database prefixes.
const unsupportedtenantIds = ['', tenantId, 'admin', 'local', 'config'];
unsupportedtenantIds.forEach((invalidTenantId) => {
    const cmd = {
        donorStartMigration: 1,
        migrationId: UUID(),
        protocol: 'shard merge',
        tenantId: invalidTenantId,
        recipientConnectionString: tenantMigrationTest.getRecipientRst().getURL(),
        readPreference,
        donorCertificateForRecipient: migrationCertificates.donorCertificateForRecipient,
        recipientCertificateForDonor: migrationCertificates.recipientCertificateForDonor,
    };
    assert.commandFailedWithCode(donorPrimary.adminCommand(cmd),
                                 [ErrorCodes.InvalidOptions, ErrorCodes.BadValue]);
});

// Test merging to the donor itself.
assert.commandFailedWithCode(donorPrimary.adminCommand({
    donorStartMigration: 1,
    migrationId: UUID(),
    protocol: 'shard merge',
    recipientConnectionString: tenantMigrationTest.getDonorRst().getURL(),
    readPreference,
    donorCertificateForRecipient: migrationCertificates.donorCertificateForRecipient,
    recipientCertificateForDonor: migrationCertificates.recipientCertificateForDonor,
}),
                             ErrorCodes.BadValue);

// Test merging to a recipient that shares one or more hosts with the donor.
assert.commandFailedWithCode(donorPrimary.adminCommand({
    donorStartMigration: 1,
    migrationId: UUID(),
    protocol: 'shard merge',
    recipientConnectionString:
        tenantMigrationTest.getRecipientRst().getURL() + "," + donorPrimary.host,
    readPreference,
    donorCertificateForRecipient: migrationCertificates.donorCertificateForRecipient,
    recipientCertificateForDonor: migrationCertificates.recipientCertificateForDonor,
}),
                             ErrorCodes.BadValue);

// Test merging to a standalone recipient.
assert.commandFailedWithCode(donorPrimary.adminCommand({
    donorStartMigration: 1,
    migrationId: UUID(),
    protocol: 'shard merge',
    recipientConnectionString: recipientPrimary.host,
    readPreference,
    donorCertificateForRecipient: migrationCertificates.donorCertificateForRecipient,
    recipientCertificateForDonor: migrationCertificates.recipientCertificateForDonor,
}),
                             ErrorCodes.BadValue);

jsTestLog("Testing 'recipientSyncData' command provided with invalid options.");

// Test erroneously included tenantId field and unsupported database prefixes.
unsupportedtenantIds.forEach((invalidTenantId) => {
    assert.commandFailedWithCode(recipientPrimary.adminCommand({
        recipientSyncData: 1,
        migrationId: UUID(),
        donorConnectionString: tenantMigrationTest.getDonorRst().getURL(),
        tenantId: invalidTenantId,
        tenantIds: [ObjectId()],
        protocol: 'shard merge',
        startMigrationDonorTimestamp: Timestamp(1, 1),
        readPreference,
        recipientCertificateForDonor: migrationCertificates.recipientCertificateForDonor,
    }),
                                 [ErrorCodes.InvalidOptions, ErrorCodes.BadValue]);
});

// Test merging from the recipient itself.
assert.commandFailedWithCode(recipientPrimary.adminCommand({
    recipientSyncData: 1,
    migrationId: UUID(),
    protocol: 'shard merge',
    tenantIds: [ObjectId()],
    donorConnectionString: tenantMigrationTest.getRecipientRst().getURL(),
    startMigrationDonorTimestamp: Timestamp(1, 1),
    readPreference,
    recipientCertificateForDonor: migrationCertificates.recipientCertificateForDonor,
}),
                             ErrorCodes.BadValue);

// Test merging from a donor that shares one or more hosts with the recipient.
assert.commandFailedWithCode(recipientPrimary.adminCommand({
    recipientSyncData: 1,
    migrationId: UUID(),
    protocol: 'shard merge',
    tenantIds: [ObjectId()],
    donorConnectionString: `${tenantMigrationTest.getDonorRst().getURL()},${recipientPrimary.host}`,
    startMigrationDonorTimestamp: Timestamp(1, 1),
    readPreference,
    recipientCertificateForDonor: migrationCertificates.recipientCertificateForDonor,
}),
                             ErrorCodes.BadValue);

// Test merging from a standalone donor.
assert.commandFailedWithCode(recipientPrimary.adminCommand({
    recipientSyncData: 1,
    migrationId: UUID(),
    protocol: 'shard merge',
    tenantIds: [ObjectId()],
    donorConnectionString: recipientPrimary.host,
    startMigrationDonorTimestamp: Timestamp(1, 1),
    readPreference,
    recipientCertificateForDonor: migrationCertificates.recipientCertificateForDonor,
}),
                             ErrorCodes.BadValue);

// Test 'returnAfterReachingDonorTimestamp' can't be null.
const nullTimestamps = [Timestamp(0, 0), Timestamp(0, 1)];
nullTimestamps.forEach((nullTs) => {
    assert.commandFailedWithCode(donorPrimary.adminCommand({
        recipientSyncData: 1,
        migrationId: UUID(),
        protocol: 'shard merge',
        tenantIds: [ObjectId()],
        donorConnectionString: tenantMigrationTest.getDonorRst().getURL(),
        startMigrationDonorTimestamp: Timestamp(1, 1),
        readPreference,
        returnAfterReachingDonorTimestamp: nullTs,
        recipientCertificateForDonor: migrationCertificates.recipientCertificateForDonor,
    }),
                                 ErrorCodes.BadValue);
});

// Test without tenantIds
assert.commandFailedWithCode(recipientPrimary.adminCommand({
    recipientSyncData: 1,
    migrationId: UUID(),
    protocol: 'shard merge',
    donorConnectionString: tenantMigrationTest.getDonorRst().getURL(),
    startMigrationDonorTimestamp: Timestamp(1, 1),
    readPreference,
    recipientCertificateForDonor: migrationCertificates.recipientCertificateForDonor,
}),
                             ErrorCodes.InvalidOptions);

// Test with an empty tenantIds list
assert.commandFailedWithCode(recipientPrimary.adminCommand({
    recipientSyncData: 1,
    migrationId: UUID(),
    protocol: 'shard merge',
    tenantIds: [],
    donorConnectionString: tenantMigrationTest.getDonorRst().getURL(),
    startMigrationDonorTimestamp: Timestamp(1, 1),
    readPreference,
    recipientCertificateForDonor: migrationCertificates.recipientCertificateForDonor,
}),
                             ErrorCodes.InvalidOptions);

// The decision field must be set for recipientForgetMigration with shard merge
assert.commandFailedWithCode(recipientPrimary.adminCommand({
    recipientForgetMigration: 1,
    protocol: "shard merge",
    migrationId: UUID(),
    tenantIds: [ObjectId()],
    donorConnectionString: tenantMigrationTest.getDonorRst().getURL(),
    readPreference,
    recipientCertificateForDonor: migrationCertificates.recipientCertificateForDonor
}),
                             ErrorCodes.InvalidOptions);

tenantMigrationTest.stop();
