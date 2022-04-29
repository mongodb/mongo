/**
 * Test a (non-prepared) large-format committed transaction T, with at least one oplog entry before
 * startFetchingDonorOpTime, and a commit OpTime between startFetchingDonorOpTime and
 * startApplyingAfterOpTime.
 *
 *                                     Donor Oplog
 *        *-------------------*-------------------*-------------------*
 *   T applyOps entry    startFetching       T commit entry      startApplying
 *                                                |
 *        <-------------- prevOpTime -------------*
 *
 * The recipient doesn't need to recover T's oplog chain, since T committed before startApplying,
 * and trying to recover T would fail because the recipient didn't fetch T's oldest entries.
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

const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});

const tenantId = "testTenantId";
const tenantDB = tenantMigrationTest.tenantDB(tenantId, "testDB");
const collName = "testColl";
const tenantNS = `${tenantDB}.${collName}`;
const transactionsNS = "config.transactions";

const donorPrimary = tenantMigrationTest.getDonorPrimary();
const recipientPrimary = tenantMigrationTest.getRecipientPrimary();

jsTestLog("Running a migration");
const migrationId = UUID();
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(migrationId),
    tenantId,
};

jsTestLog("Hang just before getting start opTimes from donor");
let fp =
    configureFailPoint(recipientPrimary, "fpAfterComparingRecipientAndDonorFCV", {action: "hang"});
tenantMigrationTest.startMigration(migrationOpts);

fp.wait();

{
    // This transaction will straddle startFetching - the oplog entry for the commit will have a
    // timestamp equal to startFetching, and previous entries will have timestamps earlier than it.
    jsTestLog("Run and commit a transaction prior to the migration");
    const session = donorPrimary.startSession({causalConsistency: false});
    const sessionDb = session.getDatabase(tenantDB);
    const sessionColl = sessionDb.getCollection(collName);

    function makeLargeDoc(numMB) {
        return {x: new Array(numMB * 1024 * 1024).join('A')};
    }

    session.startTransaction();
    sessionColl.insert({doc: makeLargeDoc(10)});
    sessionColl.insert({doc: makeLargeDoc(5)});
    sessionColl.insert({doc: makeLargeDoc(5)});
    let commitRes = session.commitTransaction_forTesting();
    assert.eq(1, commitRes.ok);
    session.endSession();
}

jsTestLog("LastWriteOpTime of transaction is " +
          tojson(donorPrimary.getCollection(transactionsNS)
                     .find({}, {"_id": -1, "lastWriteOpTime": 1})
                     .toArray()));

fp.off();
TenantMigrationTest.assertCommitted(tenantMigrationTest.waitForMigrationToComplete(migrationOpts));
assert.eq(recipientPrimary.getCollection(tenantNS).countDocuments({}), 3);
assert.eq(recipientPrimary.getCollection(tenantNS).count(), 3);
tenantMigrationTest.stop();
})();
