/**
 * Tests that the donorStartMigration & recipientSyncData command throws an error if the provided
 * tenantId is unsupported (i.e. '', 'admin', 'local' or 'config') or if the recipient
 * connection string matches the donor's connection string.
 *
 * @tags: [requires_fcv_47]
 */

(function() {
"use strict";

load("jstests/replsets/libs/tenant_migration_util.js");

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();
if (!TenantMigrationUtil.isFeatureFlagEnabled(rst.getPrimary())) {
    jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
    rst.stopSet();
    return;
}
const primary = rst.getPrimary();
const tenantId = "test";
const connectionString = "foo/bar:12345";
const readPreference = {
    mode: 'primary'
};

jsTestLog("Testing 'donorStartMigration' command provided with invalid options.");

// Test unsupported database prefixes.
const unsupportedtenantIds = ['', 'admin', 'local', 'config'];
unsupportedtenantIds.forEach((invalidTenantId) => {
    assert.commandFailedWithCode(primary.adminCommand({
        donorStartMigration: 1,
        migrationId: UUID(),
        recipientConnectionString: connectionString,
        tenantId: invalidTenantId,
        readPreference: readPreference
    }),
                                 ErrorCodes.BadValue);
});

// Test migrating a database to the donor itself.
assert.commandFailedWithCode(primary.adminCommand({
    donorStartMigration: 1,
    migrationId: UUID(),
    recipientConnectionString: rst.getURL(),
    tenantId: tenantId,
    readPreference: readPreference
}),
                             ErrorCodes.BadValue);

// Test migrating a database to a recipient that has one or more same hosts as donor.
const conflictingRecipientConnectionString = connectionString + "," + primary.host;
assert.commandFailedWithCode(primary.adminCommand({
    donorStartMigration: 1,
    migrationId: UUID(),
    recipientConnectionString: conflictingRecipientConnectionString,
    tenantId: tenantId,
    readPreference: readPreference
}),
                             ErrorCodes.BadValue);

jsTestLog("Testing 'recipientSyncData' command provided with invalid options.");

// Test unsupported database prefixes.
unsupportedtenantIds.forEach((invalidTenantId) => {
    assert.commandFailedWithCode(primary.adminCommand({
        recipientSyncData: 1,
        migrationId: UUID(),
        donorConnectionString: connectionString,
        tenantId: invalidTenantId,
        readPreference: readPreference
    }),
                                 ErrorCodes.BadValue);
});

// Test migrating a database from recipient itself.
assert.commandFailedWithCode(primary.adminCommand({
    recipientSyncData: 1,
    migrationId: UUID(),
    donorConnectionString: rst.getURL(),
    tenantId: tenantId,
    readPreference: readPreference
}),
                             ErrorCodes.BadValue);

// Test migrating a database from a donor that has one or more same hosts as recipient.
const conflictingDonorConnectionString = connectionString + "," + primary.host;
assert.commandFailedWithCode(primary.adminCommand({
    recipientSyncData: 1,
    migrationId: UUID(),
    donorConnectionString: conflictingDonorConnectionString,
    tenantId: tenantId,
    readPreference: readPreference
}),
                             ErrorCodes.BadValue);

// Test 'returnAfterReachingDonorTimestamp' can' be null.
const nullTimestamps = [Timestamp(0, 0), Timestamp(0, 1)];
nullTimestamps.forEach((nullTs) => {
    assert.commandFailedWithCode(primary.adminCommand({
        recipientSyncData: 1,
        migrationId: UUID(),
        donorConnectionString: connectionString,
        tenantId: tenantId,
        readPreference: readPreference,
        returnAfterReachingDonorTimestamp: nullTs
    }),
                                 ErrorCodes.BadValue);
});

rst.stopSet();
})();