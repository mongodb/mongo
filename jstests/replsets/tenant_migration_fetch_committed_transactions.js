/**
 * Tests that the migration recipient will retrieve committed transactions on the donor with a
 * 'lastWriteOpTime' before the stored 'startFetchingOpTime'. The recipient should store these
 * committed transaction entries in its own 'config.transactions' collection.
 *
 * @tags: [requires_fcv_49, requires_majority_read_concern, incompatible_with_eft,
 * incompatible_with_windows_tls]
 */

(function() {
"use strict";

load("jstests/core/txns/libs/prepare_helpers.js");
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");
load("jstests/replsets/rslib.js");
load("jstests/libs/uuid_util.js");

const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});
if (!tenantMigrationTest.isFeatureFlagEnabled()) {
    jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
    return;
}

const tenantId = "testTenantId";
const tenantDB = tenantMigrationTest.tenantDB(tenantId, "testDB");
const nonTenantDB = tenantMigrationTest.nonTenantDB(tenantId, "testDB");
const collName = "testColl";
const tenantNS = `${tenantDB}.${collName}`;
const transactionsNS = "config.transactions";

const donorPrimary = tenantMigrationTest.getDonorPrimary();
const recipientPrimary = tenantMigrationTest.getRecipientPrimary();

assert.commandWorked(donorPrimary.getCollection(tenantNS).insert([{_id: 0, x: 0}, {_id: 1, x: 1}],
                                                                 {writeConcern: {w: "majority"}}));

{
    jsTestLog("Run and commit a transaction prior to the migration");
    const session = donorPrimary.startSession({causalConsistency: false});
    const sessionDb = session.getDatabase(tenantDB);
    const sessionColl = sessionDb.getCollection(collName);

    session.startTransaction({writeConcern: {w: "majority"}});
    const findAndModifyRes0 = sessionColl.findAndModify({query: {x: 0}, remove: true});
    assert.eq({_id: 0, x: 0}, findAndModifyRes0);
    assert.commandWorked(session.commitTransaction_forTesting());
    assert.sameMembers(sessionColl.find({}).toArray(), [{_id: 1, x: 1}]);
    session.endSession();
}

// This should be the only transaction entry on the donor fetched by the recipient.
const fetchedDonorTxnEntry = donorPrimary.getCollection(transactionsNS).find().toArray();

{
    jsTestLog("Run and abort a transaction prior to the migration");
    const session = donorPrimary.startSession({causalConsistency: false});
    const sessionDb = session.getDatabase(tenantDB);
    const sessionColl = sessionDb.getCollection(collName);

    session.startTransaction({writeConcern: {w: "majority"}});
    const findAndModifyRes0 = sessionColl.findAndModify({query: {x: 1}, remove: true});
    assert.eq({_id: 1, x: 1}, findAndModifyRes0);

    // We prepare the transaction so that 'abortTransaction' will update the transactions table. We
    // should later see that the recipient will not update its transactions table with this entry,
    // since we only fetch committed transactions.
    PrepareHelpers.prepareTransaction(session);

    assert.commandWorked(session.abortTransaction_forTesting());
    assert.sameMembers(sessionColl.find({}).toArray(), [{_id: 1, x: 1}]);
    session.endSession();
}

{
    jsTestLog("Run and commit a transaction that does not belong to the tenant");
    const session = donorPrimary.startSession({causalConsistency: false});
    const sessionDb = session.getDatabase(nonTenantDB);
    const sessionColl = sessionDb.getCollection(collName);

    session.startTransaction({writeConcern: {w: "majority"}});
    assert.commandWorked(sessionColl.insert([{_id: 0, x: 0}, {_id: 1, x: 1}]));
    assert.commandWorked(session.commitTransaction_forTesting());
    session.endSession();
}

const fpAfterRetrievingStartOpTime = configureFailPoint(
    recipientPrimary, "fpAfterRetrievingStartOpTimesMigrationRecipientInstance", {action: "hang"});

jsTestLog("Starting a migration");
const migrationId = UUID();
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(migrationId),
    tenantId,
};
assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));
fpAfterRetrievingStartOpTime.wait();

{
    jsTestLog("Run and commit a transaction in the middle of the migration");
    const session = donorPrimary.startSession({causalConsistency: false});
    const sessionDb = session.getDatabase(tenantDB);
    const sessionColl = sessionDb.getCollection(collName);

    session.startTransaction({writeConcern: {w: "majority"}});
    assert.commandWorked(sessionColl.insert([{_id: 2, x: 2}, {_id: 3, x: 3}]));
    assert.commandWorked(session.commitTransaction_forTesting());
    session.endSession();
}

jsTestLog("Waiting for migration to complete");
fpAfterRetrievingStartOpTime.off();
assert.commandWorked(tenantMigrationTest.waitForMigrationToComplete(migrationOpts));

const donorTxnEntries = donorPrimary.getCollection(transactionsNS).find().toArray();
jsTestLog(`All donor entries: ${tojson(donorTxnEntries)}`);
assert.eq(4, donorTxnEntries.length, `donor transaction entries: ${tojson(donorTxnEntries)}`);

const recipientTxnEntries = recipientPrimary.getCollection(transactionsNS).find().toArray();

// Verify that the recipient has fetched and written only the first committed transaction entry from
// the donor.
assert.eq(
    1, recipientTxnEntries.length, `recipient transaction entries: ${tojson(recipientTxnEntries)}`);
assert.eq(
    fetchedDonorTxnEntry,
    recipientTxnEntries,
    `fetched donor transaction entries: ${
        tojson(
            fetchedDonorTxnEntry)}; recipient transaction entries: ${tojson(recipientTxnEntries)}`);

// Test that the client can retry 'commitTransaction' on the recipient.
const recipientTxnEntry = recipientTxnEntries[0];
assert.commandWorked(recipientPrimary.adminCommand({
    commitTransaction: 1,
    lsid: recipientTxnEntry._id,
    txnNumber: recipientTxnEntry.txnNum,
    autocommit: false,
}));

tenantMigrationTest.stop();
})();
