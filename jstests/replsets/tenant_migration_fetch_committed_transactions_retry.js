/**
 * Tests the retry logic for fetching committed transactions. We test the following scenarios:
 *
 * 1) Retrying after the migration has already fetched and updated its transactions entries.
 * 2) Retrying while the migration is actively updating its transactions entries.
 * 3) Retrying while the migration is updating, and the donor starts a new transaction on an
 *    existing session.
 * *
 * @tags: [
 *   incompatible_with_macos,
 *   # Shard merge is not resilient to donor restarts.
 *   incompatible_with_shard_merge,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 * ]
 */

import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";

load("jstests/aggregation/extras/utils.js");
load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");

let tenantMigrationTest = new TenantMigrationTest({name: jsTestName(), sharedOptions: {nodes: 1}});

const tenantId = ObjectId().str;
const collName = "testColl";
const transactionsNS = "config.transactions";

const runTransaction = (donorPrimary, tenantDB, collName, session) => {
    jsTestLog("Run and commit a transaction prior to the migration");
    if (!session) {
        session = donorPrimary.startSession({causalConsistency: false});
    }
    const sessionDb = session.getDatabase(tenantDB);
    const sessionColl = sessionDb.getCollection(collName);

    session.startTransaction({writeConcern: {w: "majority"}});
    sessionColl.insert({x: 0});
    assert.commandWorked(session.commitTransaction_forTesting());
    session.endSession();

    return donorPrimary.getCollection(transactionsNS).find().toArray();
};

const assertTransactionEntries = (donorTxnEntries, recipientTxnEntries) => {
    assert.eq(
        donorTxnEntries.length,
        recipientTxnEntries.length,
        `donor txn entries: ${donorTxnEntries}; recipient txn entries: ${recipientTxnEntries}`);
    for (const entry of [...donorTxnEntries, ...recipientTxnEntries]) {
        // We expect the following fields to be overwritten by the recipient, so we can remove
        // them when comparing entries between the donor and recipient.
        delete entry.lastWriteOpTime;
        delete entry.lastWriteDate;
    }
    assertArrayEq({actual: donorTxnEntries, expected: recipientTxnEntries});
};

(() => {
    jsTestLog("Test retrying after successfully updating entries");

    const tenantDB = tenantMigrationTest.tenantDB(tenantId, "testDB");
    const donorRst = tenantMigrationTest.getDonorRst();
    const donorPrimary = tenantMigrationTest.getDonorPrimary();
    const recipientPrimary = tenantMigrationTest.getRecipientPrimary();
    const recipientConfigColl =
        recipientPrimary.getCollection(TenantMigrationTest.kConfigRecipientsNS);

    const donorTxnEntries = runTransaction(donorPrimary, tenantDB, collName);
    assert.eq(1, donorTxnEntries.length);
    const donorTxnEntry = donorTxnEntries[0];

    // Hang the migration after it has fetched and updated its 'config.transactions' entries.
    const fpAfterFetchingCommittedTransactions = configureFailPoint(
        recipientPrimary, "fpAfterFetchingCommittedTransactions", {action: "hang"});

    jsTestLog("Starting a migration");
    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId,
    };
    assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));

    fpAfterFetchingCommittedTransactions.wait();

    // Verify that the transaction was updated correctly.
    let recipientTxnEntries = recipientPrimary.getCollection(transactionsNS).find().toArray();
    assert.eq(1, recipientTxnEntries.length);
    const recipientTxnEntry = recipientTxnEntries[0];
    assert.eq(donorTxnEntry._id, recipientTxnEntry._id);
    assert.eq(donorTxnEntry.txnNum, recipientTxnEntry.txnNum);

    // Restart the donor primary. This will cause the migration to restart.
    jsTestLog("Restarting donor primary");
    donorRst.restart(donorPrimary);

    // Let the migration restart and hang before it tries to re-fetch committed transactions.
    const fpBeforeFetchingTransactions = configureFailPoint(
        recipientPrimary, "fpBeforeFetchingCommittedTransactions", {action: "hang"});
    fpAfterFetchingCommittedTransactions.off();
    fpBeforeFetchingTransactions.wait();

    // The recipient should indicate that the migration has restarted.
    let recipientDoc;
    assert.soon(() => {
        recipientDoc = recipientConfigColl.find({"_id": migrationId}).toArray();
        return recipientDoc[0].numRestartsDueToDonorConnectionFailure == 1;
    });
    // The state doc should indicate that the migration has already updated 'config.transaction'
    // entries.
    assert.eq(true, recipientDoc[0].completedUpdatingTransactionsBeforeStartOpTime);
    fpBeforeFetchingTransactions.off();

    // Verify that the migration completes successfully.
    TenantMigrationTest.assertCommitted(
        tenantMigrationTest.waitForMigrationToComplete(migrationOpts));
    recipientTxnEntries = recipientPrimary.getCollection(transactionsNS).find().toArray();
    assertTransactionEntries(donorTxnEntries, recipientTxnEntries);

    tenantMigrationTest.stop();
})();

(() => {
    jsTestLog("Test retrying in the middle of updating entries");

    tenantMigrationTest = new TenantMigrationTest({name: jsTestName(), sharedOptions: {nodes: 1}});
    const tenantDB = tenantMigrationTest.tenantDB(tenantId, "testDB");
    const collName1 = `${collName}1`;
    const collName2 = `${collName}2`;

    const donorRst = tenantMigrationTest.getDonorRst();
    const donorPrimary = tenantMigrationTest.getDonorPrimary();
    const recipientPrimary = tenantMigrationTest.getRecipientPrimary();
    const recipientConfigColl =
        recipientPrimary.getCollection(TenantMigrationTest.kConfigRecipientsNS);

    runTransaction(donorPrimary, tenantDB, collName1);
    const donorTxnEntries = runTransaction(donorPrimary, tenantDB, collName2);
    assert.eq(2, donorTxnEntries.length);

    // Hang the recipient after it updates the first transaction entry. Have the failpoint throw
    // a retriable error to avoid the potential race condition where the recipient continues
    // processing transaction entries in the same batch after the failpoint is released.
    const hangAfterUpdatingTransactionEntry = configureFailPoint(
        recipientPrimary, "hangAfterUpdatingTransactionEntry", {"failAfterHanging": true});

    jsTestLog("Starting a migration");
    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId,
    };
    assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));

    hangAfterUpdatingTransactionEntry.wait();

    // Verify that only one transaction was updated correctly.
    let recipientTxnEntries = recipientPrimary.getCollection(transactionsNS).find().toArray();
    assert.eq(1, recipientTxnEntries.length);

    // Restart the donor primary. This will cause the migration to restart.
    jsTestLog("Restarting donor primary");
    donorRst.restart(donorPrimary);

    // Let the migration restart and hang before it tries to re-fetch committed transactions.
    const fpBeforeFetchingTransactions = configureFailPoint(
        recipientPrimary, "fpBeforeFetchingCommittedTransactions", {action: "hang"});
    hangAfterUpdatingTransactionEntry.off();
    fpBeforeFetchingTransactions.wait();

    // The recipient should indicate that the migration has restarted.
    let recipientDoc;
    assert.soon(() => {
        recipientDoc = recipientConfigColl.find({"_id": migrationId}).toArray();
        return recipientDoc[0].numRestartsDueToDonorConnectionFailure == 1;
    });
    // Verify that the 'completedUpdatingTransactionsBeforeStartOpTime' flag is false since the
    // migration was forced to restart before it fully completed fetching.
    assert.eq(false, recipientDoc[0].completedUpdatingTransactionsBeforeStartOpTime);
    fpBeforeFetchingTransactions.off();

    // Verify that the migration completes successfully.
    TenantMigrationTest.assertCommitted(
        tenantMigrationTest.waitForMigrationToComplete(migrationOpts));

    recipientTxnEntries = recipientPrimary.getCollection(transactionsNS).find().toArray();
    assertTransactionEntries(donorTxnEntries, recipientTxnEntries);

    tenantMigrationTest.stop();
})();

(() => {
    jsTestLog("Test retrying with a new transaction in the middle of updating entries");

    tenantMigrationTest = new TenantMigrationTest({name: jsTestName(), sharedOptions: {nodes: 1}});
    const tenantDB = tenantMigrationTest.tenantDB(tenantId, "testDB");
    const collName1 = `${collName}1`;
    const collName2 = `${collName}2`;

    const donorRst = tenantMigrationTest.getDonorRst();
    const donorPrimary = tenantMigrationTest.getDonorPrimary();
    const recipientPrimary = tenantMigrationTest.getRecipientPrimary();
    const recipientConfigColl =
        recipientPrimary.getCollection(TenantMigrationTest.kConfigRecipientsNS);

    runTransaction(donorPrimary, tenantDB, collName1);
    // Start a session on the donor. We will run another transaction on this session later.
    const session = donorPrimary.startSession({causalConsistency: false});
    const initialDonorTxnEntries = runTransaction(donorPrimary, tenantDB, collName2, session);
    assert.eq(2, initialDonorTxnEntries.length);

    // Hang the recipient after it updates the first transaction entry. Have the failpoint throw
    // a retriable error to avoid the potential race condition where the recipient continues
    // processing transaction entries in the same batch after the failpoint is released.
    const hangAfterUpdatingTransactionEntry = configureFailPoint(
        recipientPrimary, "hangAfterUpdatingTransactionEntry", {"failAfterHanging": true});

    jsTestLog("Starting a migration");
    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId,
    };
    assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));

    hangAfterUpdatingTransactionEntry.wait();

    // Verify that the recipient receives one of the donor transaction entries.
    let recipientTxnEntries = recipientPrimary.getCollection(transactionsNS).find().toArray();
    assert.eq(1, recipientTxnEntries.length);

    // Run a new transaction with the same session on the donor. This will advance its
    // 'lastWriteOpTime' past the recipient's 'startFetchingDonorOpTime'.
    // Note: This update should be applied via the recipient tenant oplog applier instead of the
    // fetch transactions stage.
    const updatedDonorTxnEntries = runTransaction(donorPrimary, tenantDB, collName2, session);
    assert.eq(2, updatedDonorTxnEntries.length);
    assert.eq(false, arrayEq(initialDonorTxnEntries, updatedDonorTxnEntries));

    // Restart the donor primary. This will cause the migration to restart.
    jsTestLog("Restarting donor primary");
    donorRst.restart(donorPrimary);

    // Let the migration restart and hang before it tries to re-fetch committed transactions.
    const fpBeforeFetchingTransactions = configureFailPoint(
        recipientPrimary, "fpBeforeFetchingCommittedTransactions", {action: "hang"});
    hangAfterUpdatingTransactionEntry.off();
    fpBeforeFetchingTransactions.wait();

    // The recipient should indicate that the migration has restarted.
    let recipientDoc;
    assert.soon(() => {
        recipientDoc = recipientConfigColl.find({"_id": migrationId}).toArray();
        return recipientDoc[0].numRestartsDueToDonorConnectionFailure == 1;
    });
    // Verify that the 'completedUpdatingTransactionsBeforeStartOpTime' flag is false since the
    // migration was forced to restart before it fully completed fetching.
    assert.eq(false, recipientDoc[0].completedUpdatingTransactionsBeforeStartOpTime);
    fpBeforeFetchingTransactions.off();

    // Verify that the migration completes successfully.
    TenantMigrationTest.assertCommitted(
        tenantMigrationTest.waitForMigrationToComplete(migrationOpts));

    recipientTxnEntries = recipientPrimary.getCollection(transactionsNS).find().toArray();
    assertTransactionEntries(updatedDonorTxnEntries, recipientTxnEntries);

    tenantMigrationTest.stop();
})();
