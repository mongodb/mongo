/**
 * Tests that the client can retry commitTransaction on the tenant migration recipient.
 *
 * @tags: [requires_fcv_49, requires_majority_read_concern, incompatible_with_eft,
 * incompatible_with_windows_tls]
 */

(function() {
"use strict";

load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");
load("jstests/replsets/rslib.js");
load("jstests/libs/uuid_util.js");

const migrationX509Options = TenantMigrationUtil.makeX509OptionsForTest();
const kGarbageCollectionParams = {
    // Set the delay before a donor state doc is garbage collected to be short to speed up
    // the test.
    tenantMigrationGarbageCollectionDelayMS: 3 * 1000,

    // Set the TTL monitor to run at a smaller interval to speed up the test.
    ttlMonitorSleepSecs: 1,
};

const donorRst = new ReplSetTest({
    nodes: 1,
    name: "donor",
    nodeOptions: Object.assign(migrationX509Options.donor, {setParameter: kGarbageCollectionParams})
});
const recipientRst = new ReplSetTest({
    nodes: [{}, {rsConfig: {priority: 0}}, {rsConfig: {priority: 0}}],
    name: "recipient",
    nodeOptions:
        Object.assign(migrationX509Options.recipient, {setParameter: kGarbageCollectionParams})
});

donorRst.startSet();
donorRst.initiate();

recipientRst.startSet();
recipientRst.initiate();

const tenantMigrationTest = new TenantMigrationTest({name: jsTestName(), donorRst, recipientRst});
if (!tenantMigrationTest.isFeatureFlagEnabled()) {
    jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
    donorRst.stopSet();
    recipientRst.stopSet();
    return;
}

const kTenantId = "testTenantId";
const kDbName = tenantMigrationTest.tenantDB(kTenantId, "testDB");
const kCollName = "testColl";
const kNs = `${kDbName}.${kCollName}`;

const donorPrimary = donorRst.getPrimary();
const recipientPrimary = recipientRst.getPrimary();

assert.commandWorked(donorPrimary.getCollection(kNs).insert(
    [{_id: 0, x: 0}, {_id: 1, x: 1}, {_id: 2, x: 2}], {writeConcern: {w: "majority"}}));

{
    jsTest.log("Run a transaction prior to the migration");
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

const waitAfterStartingOplogApplier = configureFailPoint(
    recipientPrimary, "fpAfterStartingOplogApplierMigrationRecipientInstance", {action: "hang"});

jsTest.log("Run a migration to completion");
const migrationId = UUID();
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(migrationId),
    tenantId: kTenantId,
};
tenantMigrationTest.startMigration(migrationOpts);

// Hang the recipient during oplog application before we continue to run more transactions on the
// donor. This is to test applying multiple transactions on multiple sessions in the same batch.
waitAfterStartingOplogApplier.wait();
const waitInOplogApplier = configureFailPoint(recipientPrimary, "hangInTenantOplogApplication");
tenantMigrationTest.insertDonorDB(kDbName, kCollName, [{_id: 3, x: 3}, {_id: 4, x: 4}]);

waitInOplogApplier.wait();

jsTestLog("Run transactions while the migration is running");
// Run transactions against the donor on different sessions.
for (let i = 0; i < 10; i++) {
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

waitAfterStartingOplogApplier.off();
waitInOplogApplier.off();

assert.commandWorked(tenantMigrationTest.waitForMigrationToComplete(migrationOpts));
tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString);
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

donorRst.stopSet();
recipientRst.stopSet();
})();
