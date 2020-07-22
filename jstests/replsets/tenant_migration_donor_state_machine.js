/**
 * Tests that after donorStartCommand is run, that reads and writes should be blocked for the
 * migrating tenant.
 * @tags: [requires_fcv_46]
 */

(function() {
"use strict";

// An object that mirrors the access states for the MigratingTenantAccessBlockers.
const accessState = {
    kAllow: 0,
    kBlockingWrites: 1,
    kBlockingReadsAndWrites: 2,
    kReject: 3
};

const donorRst = new ReplSetTest({nodes: 1});
const recipientRst = new ReplSetTest({nodes: 1});

donorRst.startSet();
donorRst.initiate();

const donorPrimary = donorRst.getPrimary();

const kMigrationId = new UUID();
const kRecipientConnectionString = recipientRst.getURL();

const kReadPreference = {
    mode: "primary"
};
const kDBPrefix = 'databaseABC';

jsTest.log('Running donorStartMigration command.');
assert.commandWorked(donorPrimary.adminCommand({
    donorStartMigration: 1,
    migrationId: kMigrationId,
    recipientConnectionString: kRecipientConnectionString,
    databasePrefix: kDBPrefix,
    readPreference: kReadPreference
}));

jsTest.log('Running the serverStatus command.');
const migratingTenantServerStatus =
    donorPrimary.adminCommand({serverStatus: 1}).migratingTenantAccessBlocker;

jsTest.log(tojson(migratingTenantServerStatus));
// The donorStartMigration does the blocking write after updating the in-memory
// access state to kBlockingWrites, and on completing the write the access state is
// updated to kBlockingReadsAndWrites. Since the command doesn't return until the
// write is completed, the state is always kBlockingReadsAndWrites after the
// command returns.
assert.eq(migratingTenantServerStatus[kDBPrefix].access, accessState.kBlockingReadsAndWrites);
assert(migratingTenantServerStatus[kDBPrefix].blockTimestamp);

donorRst.stopSet();
})();
