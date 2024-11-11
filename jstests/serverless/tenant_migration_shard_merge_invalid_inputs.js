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
 *   requires_shard_merge,
 *   requires_persistence,
 *   serverless,
 *   requires_fcv_71,
 * ]
 */

import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";
import {
    kProtocolShardMerge,
} from "jstests/replsets/libs/tenant_migration_util.js";

const tenantMigrationTest =
    new TenantMigrationTest({name: jsTestName(), enableRecipientTesting: false});

const donorPrimary = tenantMigrationTest.getDonorPrimary();

const recipientPrimary = tenantMigrationTest.getRecipientPrimary();

const tenantId = ObjectId().str;
const readPreference = {
    mode: 'primary'
};

jsTestLog("Testing 'donorStartMigration' command provided with invalid options.");

// Test erroneously included tenantId field and unsupported database prefixes.
const unsupportedtenantIds = ['', tenantId, 'admin', 'local', 'config'];
unsupportedtenantIds.forEach((invalidTenantId) => {
    const cmd = {
        donorStartMigration: 1,
        migrationId: UUID(),
        protocol: kProtocolShardMerge,
        tenantId: invalidTenantId,
        recipientConnectionString: tenantMigrationTest.getRecipientRst().getURL(),
        readPreference,
    };
    assert.commandFailedWithCode(donorPrimary.adminCommand(cmd),
                                 [ErrorCodes.InvalidOptions, ErrorCodes.BadValue]);
});

// Test merging to the donor itself.
assert.commandFailedWithCode(donorPrimary.adminCommand({
    donorStartMigration: 1,
    migrationId: UUID(),
    protocol: kProtocolShardMerge,
    recipientConnectionString: tenantMigrationTest.getDonorRst().getURL(),
    readPreference,
}),
                             ErrorCodes.BadValue);

// Test merging to a recipient that shares one or more hosts with the donor.
assert.commandFailedWithCode(donorPrimary.adminCommand({
    donorStartMigration: 1,
    migrationId: UUID(),
    protocol: kProtocolShardMerge,
    recipientConnectionString:
        tenantMigrationTest.getRecipientRst().getURL() + "," + donorPrimary.host,
    readPreference,
}),
                             ErrorCodes.BadValue);

// Test merging to a standalone recipient.
assert.commandFailedWithCode(donorPrimary.adminCommand({
    donorStartMigration: 1,
    migrationId: UUID(),
    protocol: kProtocolShardMerge,
    recipientConnectionString: recipientPrimary.host,
    readPreference,
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
        protocol: kProtocolShardMerge,
        startMigrationDonorTimestamp: Timestamp(1, 1),
        readPreference,
    }),
                                 [ErrorCodes.InvalidOptions, ErrorCodes.BadValue]);
});

// Test merging from the recipient itself.
assert.commandFailedWithCode(recipientPrimary.adminCommand({
    recipientSyncData: 1,
    migrationId: UUID(),
    protocol: kProtocolShardMerge,
    tenantIds: [ObjectId()],
    donorConnectionString: tenantMigrationTest.getRecipientRst().getURL(),
    startMigrationDonorTimestamp: Timestamp(1, 1),
    readPreference,
}),
                             ErrorCodes.BadValue);

// Test merging from a donor that shares one or more hosts with the recipient.
assert.commandFailedWithCode(recipientPrimary.adminCommand({
    recipientSyncData: 1,
    migrationId: UUID(),
    protocol: kProtocolShardMerge,
    tenantIds: [ObjectId()],
    donorConnectionString: `${tenantMigrationTest.getDonorRst().getURL()},${recipientPrimary.host}`,
    startMigrationDonorTimestamp: Timestamp(1, 1),
    readPreference,
}),
                             ErrorCodes.BadValue);

// Test merging from a standalone donor.
assert.commandFailedWithCode(recipientPrimary.adminCommand({
    recipientSyncData: 1,
    migrationId: UUID(),
    protocol: kProtocolShardMerge,
    tenantIds: [ObjectId()],
    donorConnectionString: recipientPrimary.host,
    startMigrationDonorTimestamp: Timestamp(1, 1),
    readPreference,
}),
                             ErrorCodes.BadValue);

// Test 'returnAfterReachingDonorTimestamp' can't be null.
const nullTimestamps = [Timestamp(0, 0), Timestamp(0, 1)];
nullTimestamps.forEach((nullTs) => {
    assert.commandFailedWithCode(donorPrimary.adminCommand({
        recipientSyncData: 1,
        migrationId: UUID(),
        protocol: kProtocolShardMerge,
        tenantIds: [ObjectId()],
        donorConnectionString: tenantMigrationTest.getDonorRst().getURL(),
        startMigrationDonorTimestamp: Timestamp(1, 1),
        readPreference,
        returnAfterReachingDonorTimestamp: nullTs,
    }),
                                 ErrorCodes.BadValue);
});

// Test without tenantIds
assert.commandFailedWithCode(recipientPrimary.adminCommand({
    recipientSyncData: 1,
    migrationId: UUID(),
    protocol: kProtocolShardMerge,
    donorConnectionString: tenantMigrationTest.getDonorRst().getURL(),
    startMigrationDonorTimestamp: Timestamp(1, 1),
    readPreference,
}),
                             ErrorCodes.InvalidOptions);

// Test with an empty tenantIds list
assert.commandFailedWithCode(recipientPrimary.adminCommand({
    recipientSyncData: 1,
    migrationId: UUID(),
    protocol: kProtocolShardMerge,
    tenantIds: [],
    donorConnectionString: tenantMigrationTest.getDonorRst().getURL(),
    startMigrationDonorTimestamp: Timestamp(1, 1),
    readPreference,
}),
                             ErrorCodes.InvalidOptions);

// The decision field must be set for recipientForgetMigration with shard merge
assert.commandFailedWithCode(recipientPrimary.adminCommand({
    recipientForgetMigration: 1,
    protocol: kProtocolShardMerge,
    migrationId: UUID(),
    tenantIds: [ObjectId()],
    donorConnectionString: tenantMigrationTest.getDonorRst().getURL(),
    readPreference,
}),
                             ErrorCodes.InvalidOptions);

tenantMigrationTest.stop();
