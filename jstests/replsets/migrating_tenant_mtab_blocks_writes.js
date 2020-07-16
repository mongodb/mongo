/**
 *
 * @tags: [requires_fcv_46]
 */

(function() {
"use strict";
const runDonorStartMigrationCommand =
    (primaryConnection, migrationId, recipientConnectionString, dbPrefix, readPreference) => {
        return primaryConnection.adminCommand({
            donorStartMigration: 1,
            migrationId,
            recipientConnectionString,
            databasePrefix: dbPrefix,
            readPreference
        });
    };

// the value of the access enum in MigratingTenantAccessBlocker for the 'kBlockWritesAndReads'
// access state.
const kBlockReadsAndWrites = 2;
const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const donorPrimary = rst.getPrimary();

const kMigrationId = new UUID();
const kRecipientConnectionString = new ReplSetTest({nodes: 1}).getURL();

const kReadPreference = {
    mode: "primary"
};
const kDBPrefixes = 'databaseABC';

jsTest.log('Running donorStartMigration command.');
assert.commandWorked(runDonorStartMigrationCommand(
    donorPrimary, kMigrationId, kRecipientConnectionString, kDBPrefixes, kReadPreference));

jsTest.log('Running the serverStatus command.');
const migratingTenantServerStatus =
    donorPrimary.adminCommand({serverStatus: 1}).migratingTenantAccessBlocker;

assert.eq(migratingTenantServerStatus.access, kBlockReadsAndWrites);
assert(migratingTenantServerStatus.blockTimestamp);

rst.stopSet();
})();