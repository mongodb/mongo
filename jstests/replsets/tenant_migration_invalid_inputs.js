/**
 * Tests that the donorStartMigration command throws a error if the provided tenantId
 * is unsupported (i.e. '', 'admin', 'local' or 'config') or if the recipient connection string
 * matches the donor's connection string.
 *
 * @tags: [requires_fcv_47]
 */

(function() {
"use strict";

const rst =
    new ReplSetTest({nodes: 1, nodeOptions: {setParameter: {enableTenantMigrations: true}}});
rst.startSet();
rst.initiate();
const primary = rst.getPrimary();

// Test unsupported tenantIds.
const unsupportedTenantIds = ['', 'admin', 'local', 'config'];

unsupportedTenantIds.forEach((tenantId) => {
    assert.commandFailedWithCode(primary.adminCommand({
        donorStartMigration: 1,
        migrationId: UUID(),
        recipientConnectionString: "testRecipientConnString",
        tenantId: tenantId,
        readPreference: {mode: "primary"}
    }),
                                 ErrorCodes.BadValue);
});

// Test migrating a database to the donor itself.
assert.commandFailedWithCode(primary.adminCommand({
    donorStartMigration: 1,
    migrationId: UUID(),
    recipientConnectionString: rst.getURL(),
    tenantId: "testTenantId",
    readPreference: {mode: "primary"}
}),
                             ErrorCodes.InvalidOptions);

// Test migrating a database to a recipient that has one or more same hosts as donor
const conflictingRecipientConnectionString = "foo/bar:12345," + primary.host;
assert.commandFailedWithCode(primary.adminCommand({
    donorStartMigration: 1,
    migrationId: UUID(),
    recipientConnectionString: conflictingRecipientConnectionString,
    tenantId: "testTenantId",
    readPreference: {mode: "primary"}
}),
                             ErrorCodes.InvalidOptions);
rst.stopSet();
})();