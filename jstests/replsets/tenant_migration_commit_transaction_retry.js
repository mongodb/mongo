/**
 * Tests that the client can retry commitTransaction on the tenant migration recipient.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 * ]
 */

(function() {
"use strict";

load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");
load("jstests/replsets/rslib.js");
load("jstests/libs/uuid_util.js");

const tenantMigrationTest = new TenantMigrationTest(
    {name: jsTestName(), sharedOptions: {nodes: 1}, quickGarbageCollection: true});

const kTenantId = "testTenantId";
const kDbName = tenantMigrationTest.tenantDB(kTenantId, "testDB");
const kCollName = "testColl";
const kNs = `${kDbName}.${kCollName}`;

const donorPrimary = tenantMigrationTest.getDonorPrimary();
const recipientPrimary = tenantMigrationTest.getRecipientPrimary();

assert.commandWorked(donorPrimary.getCollection(kNs).insert(
    [{_id: 0, x: 0}, {_id: 1, x: 1}, {_id: 2, x: 2}], {writeConcern: {w: "majority"}}));

{
    jsTestLog("Run a transaction prior to the migration");
    const session = donorPrimary.startSession({causalConsistency: false});
    const sessionDb = session.getDatabase(kDbName);
    const sessionColl = sessionDb[kCollName];

    session.startTransaction({writeConcern: {w: "majority"}});
    const findAndModifyRes0 = sessionColl.findAndModify({query: {x: 0}, remove: true});
    assert.eq({_id: 0, x: 0}, findAndModifyRes0);
    assert.commandWorked(session.commitTransaction_forTesting());
    assert.sameMembers(sessionColl.find({}).toArray(), [{_id: 1, x: 1}, {_id: 2, x: 2}]);
    session.endSession();
}

const pauseTenantMigrationBeforeLeavingDataSyncState =
    configureFailPoint(donorPrimary, "pauseTenantMigrationBeforeLeavingDataSyncState");

jsTestLog("Run a migration to completion");
const migrationId = UUID();
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(migrationId),
    tenantId: kTenantId,
};
tenantMigrationTest.startMigration(migrationOpts);

// Hang the recipient during oplog application before we continue to run more transactions on the
// donor. This is to test applying multiple transactions on multiple sessions in the same batch.
pauseTenantMigrationBeforeLeavingDataSyncState.wait();
const waitInOplogApplier = configureFailPoint(recipientPrimary, "hangInTenantOplogApplication");
tenantMigrationTest.insertDonorDB(kDbName, kCollName, [{_id: 3, x: 3}, {_id: 4, x: 4}]);

waitInOplogApplier.wait();

jsTestLog("Run transactions while the migration is running");
// Run transactions against the donor on different sessions.
for (let i = 0; i < 5; i++) {
    const session = donorPrimary.startSession();
    const sessionDb = session.getDatabase(kDbName);
    const sessionColl = sessionDb[kCollName];

    session.startTransaction({writeConcern: {w: "majority"}});
    assert.commandWorked(sessionColl.updateMany({}, {$push: {transactions: `session${i}_txn1`}}));
    assert.commandWorked(session.commitTransaction_forTesting());

    session.startTransaction({writeConcern: {w: "majority"}});
    assert.commandWorked(sessionColl.updateMany({}, {$push: {transactions: `session${i}_txn2`}}));
    assert.commandWorked(session.commitTransaction_forTesting());
    session.endSession();
}

pauseTenantMigrationBeforeLeavingDataSyncState.off();
waitInOplogApplier.off();

TenantMigrationTest.assertCommitted(tenantMigrationTest.waitForMigrationToComplete(migrationOpts));
// With `quickGarbageCollection` it's likely that forgetting the migration will race with its
// natural destruction.
assert.commandWorkedOrFailedWithCode(
    tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString),
    [ErrorCodes.NoSuchTenantMigration]);
tenantMigrationTest.waitForMigrationGarbageCollection(migrationId, kTenantId);

// Test the client can retry commitTransaction against the recipient for transactions that committed
// on the donor.
const donorTxnEntries = donorPrimary.getDB("config")["transactions"].find().toArray();
jsTestLog(`Donor config.transactions: ${tojson(donorTxnEntries)}`);
const recipientTxnEntries = recipientPrimary.getDB("config")["transactions"].find().toArray();
jsTestLog(`Recipient config.transactions: ${tojson(recipientTxnEntries)}`);
donorTxnEntries.forEach((txnEntry) => {
    jsTestLog("Retrying transaction on recipient: " + tojson(txnEntry));
    assert.commandWorked(recipientPrimary.adminCommand(
        {commitTransaction: 1, lsid: txnEntry._id, txnNumber: txnEntry.txnNum, autocommit: false}));
});

jsTestLog("Running a back-to-back migration");
const tenantMigrationTest2 = new TenantMigrationTest({
    name: jsTestName() + "2",
    donorRst: tenantMigrationTest.getRecipientRst(),
    sharedOptions: {nodes: 1},
    quickGarbageCollection: true,
});
const migrationId2 = UUID();
const migrationOpts2 = {
    migrationIdString: extractUUIDFromObject(migrationId2),
    tenantId: kTenantId,
};
TenantMigrationTest.assertCommitted(
    tenantMigrationTest2.runMigration(migrationOpts2, {enableDonorStartMigrationFsync: true}));
const recipientPrimary2 = tenantMigrationTest2.getRecipientPrimary();
const recipientTxnEntries2 = recipientPrimary2.getDB("config")["transactions"].find().toArray();
jsTestLog(`Recipient2 config.transactions: ${tojson(recipientTxnEntries2)}`);
donorTxnEntries.forEach((txnEntry) => {
    jsTestLog("Retrying transaction on recipient2 after another migration: " + tojson(txnEntry));
    assert.commandWorked(recipientPrimary2.adminCommand(
        {commitTransaction: 1, lsid: txnEntry._id, txnNumber: txnEntry.txnNum, autocommit: false}));
});
// With `quickGarbageCollection` it's likely that forgetting the migration will race with its
// natural destruction.
assert.commandWorkedOrFailedWithCode(
    tenantMigrationTest2.forgetMigration(migrationOpts2.migrationIdString),
    [ErrorCodes.NoSuchTenantMigration]);
tenantMigrationTest2.waitForMigrationGarbageCollection(migrationId2, kTenantId);

tenantMigrationTest2.stop();
tenantMigrationTest.stop();
})();
