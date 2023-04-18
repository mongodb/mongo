/**
 * Tests that retrying a failed tenant migration works even if the config.transactions on the
 * recipient is not cleaned up after the failed migration.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 * ]
 */

import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";
import {checkTenantDBHashes} from "jstests/replsets/libs/tenant_migration_util.js";

load("jstests/replsets/rslib.js");
load("jstests/libs/uuid_util.js");

const tenantMigrationTest =
    new TenantMigrationTest({name: jsTestName(), quickGarbageCollection: true});

const kTenantId = ObjectId().str;
const kDbName = tenantMigrationTest.tenantDB(kTenantId, "testDB");
const kCollName = "testColl";

const donorPrimary = tenantMigrationTest.getDonorPrimary();
const recipientPrimary = tenantMigrationTest.getRecipientPrimary();

tenantMigrationTest.insertDonorDB(kDbName, kCollName, [{_id: 1}, {_id: 2}]);

let waitBeforeFetchingTransactions =
    configureFailPoint(recipientPrimary, "fpBeforeFetchingCommittedTransactions", {action: "hang"});
// Prevent donor from blocking writes before writing the transactions (necessary for shard merge).
let pauseDonorBeforeBlocking =
    configureFailPoint(donorPrimary, "pauseTenantMigrationBeforeLeavingDataSyncState");

const migrationId = UUID();
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(migrationId),
    tenantId: kTenantId,
};
tenantMigrationTest.startMigration(migrationOpts);

waitBeforeFetchingTransactions.wait();

// Run transactions against the donor while the migration is running.
const session1 = donorPrimary.startSession();
const session2 = donorPrimary.startSession();
for (const session of [session1, session2]) {
    jsTestLog("Running transaction with session " + tojson(session.getSessionId()));
    session.startTransaction({writeConcern: {w: "majority"}});
    assert.commandWorked(session.getDatabase(kDbName)[kCollName].updateMany(
        {}, {$push: {transactions: session.getSessionId()}}));
    assert.commandWorked(session.commitTransaction_forTesting());
}

// Run retryable writes against the donor while the migration is running.
const lsid1 = {
    id: UUID()
};
const lsid2 = {
    id: UUID()
};
for (const lsid of [lsid1, lsid2]) {
    jsTestLog("Running retryable writes with lsid " + tojson(lsid));
    assert.commandWorked(donorPrimary.getDB(kDbName).runCommand({
        update: kCollName,
        updates: [
            {q: {_id: 1}, u: {$push: {retryableWrites: lsid}}},
            {q: {_id: 2}, u: {$push: {retryableWrites: lsid}}},
        ],
        txnNumber: NumberLong(0),
        lsid: lsid
    }));
}
pauseDonorBeforeBlocking.off();

// Abort the first migration.
const abortFp = configureFailPoint(donorPrimary, "abortTenantMigrationBeforeLeavingBlockingState");
waitBeforeFetchingTransactions.off();

TenantMigrationTest.assertAborted(tenantMigrationTest.waitForMigrationToComplete(migrationOpts));
tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString);
tenantMigrationTest.waitForMigrationGarbageCollection(migrationId, kTenantId);

abortFp.off();

// Clean up tenant data after a failed migration.
assert.commandWorked(recipientPrimary.getDB(kDbName).dropDatabase());

waitBeforeFetchingTransactions =
    configureFailPoint(recipientPrimary, "fpBeforeFetchingCommittedTransactions", {action: "hang"});
pauseDonorBeforeBlocking =
    configureFailPoint(donorPrimary, "pauseTenantMigrationBeforeLeavingDataSyncState");

// Retry the migration.
tenantMigrationTest.startMigration(migrationOpts);

waitBeforeFetchingTransactions.wait();

// Run a newer transaction on session2 during the migration.
session2.startTransaction({writeConcern: {w: "majority"}});
assert.commandWorked(session2.getDatabase(kDbName)[kCollName].updateMany(
    {}, {$push: {retryMigration: session2.getSessionId()}}));
assert.commandWorked(session2.commitTransaction_forTesting());

// Run a newer retryable write with lsid2 during the migration.
assert.commandWorked(donorPrimary.getDB(kDbName).runCommand({
    update: kCollName,
    updates: [
        {q: {_id: 1}, u: {$push: {retryMigration: lsid2}}},
        {q: {_id: 2}, u: {$push: {retryMigration: lsid2}}},
    ],
    txnNumber: NumberLong(1),
    lsid: lsid2
}));

waitBeforeFetchingTransactions.off();
pauseDonorBeforeBlocking.off();

TenantMigrationTest.assertCommitted(tenantMigrationTest.waitForMigrationToComplete(migrationOpts));

// Retrying commitTransaction against the recipient.
assert.commandWorked(recipientPrimary.adminCommand({
    commitTransaction: 1,
    lsid: session1.getSessionId(),
    txnNumber: NumberLong(0),
    autocommit: false
}));
assert.commandWorked(recipientPrimary.adminCommand({
    commitTransaction: 1,
    lsid: session2.getSessionId(),
    txnNumber: NumberLong(1),
    autocommit: false
}));

// Retrying retryable writes against the recipient.
assert.commandWorked(recipientPrimary.getDB(kDbName).runCommand({
    update: kCollName,
    updates: [
        {q: {_id: 1}, u: {$push: {retryableWrites: lsid1}}},
        {q: {_id: 2}, u: {$push: {retryableWrites: lsid1}}},
    ],
    txnNumber: NumberLong(0),
    lsid: lsid1
}));
assert.commandWorked(recipientPrimary.getDB(kDbName).runCommand({
    update: kCollName,
    updates: [
        {q: {_id: 1}, u: {$push: {retryMigration: lsid2}}},
        {q: {_id: 2}, u: {$push: {retryMigration: lsid2}}},
    ],
    txnNumber: NumberLong(1),
    lsid: lsid2
}));

// The dbhash between the donor and the recipient should still match after retrying
// commitTransaction and the retryable writes because they should be noop.
checkTenantDBHashes({
    donorRst: tenantMigrationTest.getDonorRst(),
    recipientRst: tenantMigrationTest.getRecipientRst(),
    tenantId: kTenantId
});

tenantMigrationTest.stop();
