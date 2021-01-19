/**
 * Tests the currentOp command during a tenant migration. A tenant migration is started, and the
 * currentOp command is tested as the recipient moves through the kStarted, kConsistent and kDone
 * states.
 *
 * Tenant migrations are not expected to be run on servers with ephemeralForTest.
 *
 * @tags: [requires_fcv_49, requires_majority_read_concern, requires_persistence,
 * incompatible_with_eft, incompatible_with_windows_tls]
 */

(function() {

"use strict";
load("jstests/libs/uuid_util.js");        // For extractUUIDFromObject().
load("jstests/libs/fail_point_util.js");  // For configureFailPoint().
load("jstests/libs/parallelTester.js");   // For the Thread().
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");

const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});
if (!tenantMigrationTest.isFeatureFlagEnabled()) {
    jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
    return;
}

// An object that mirrors the recipient migration states.
const migrationStates = {
    kStarted: 1,
    kConsistent: 2,
    kDone: 3
};

const kMigrationId = UUID();
const kTenantId = 'testTenantId';
const kReadPreference = {
    mode: "primary"
};
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(kMigrationId),
    tenantId: kTenantId,
    readPreference: kReadPreference
};

const recipientPrimary = tenantMigrationTest.getRecipientPrimary();

// Makes sure the fields that are always expected to exist, such as the donorConnectionString, are
// correct.
function checkStandardFieldsOK(res) {
    assert.eq(res.inprog.length, 1, tojson(res));
    assert.eq(bsonWoCompare(res.inprog[0].instanceID.uuid, kMigrationId), 0, tojson(res));
    assert.eq(bsonWoCompare(res.inprog[0].tenantId, kTenantId), 0, tojson(res));
    assert.eq(res.inprog[0].donorConnectionString,
              tenantMigrationTest.getDonorRst().getURL(),
              tojson(res));
    assert.eq(bsonWoCompare(res.inprog[0].readPreference, kReadPreference), 0, tojson(res));
}

// Set all failPoints up on the recipient's end to block the migration at certain points. The
// migration will be unblocked through the test to allow transitions to different states.
jsTestLog("Setting up all failPoints.");

const fpAfterPersistingStateDoc =
    configureFailPoint(recipientPrimary,
                       "fpAfterPersistingTenantMigrationRecipientInstanceStateDoc",
                       {action: "hang"});
const fpAfterRetrievingStartOpTime = configureFailPoint(
    recipientPrimary, "fpAfterRetrievingStartOpTimesMigrationRecipientInstance", {action: "hang"});
const fpAfterCollectionCloner =
    configureFailPoint(recipientPrimary, "fpAfterCollectionClonerDone", {action: "hang"});
const fpAfterDataConsistent = configureFailPoint(
    recipientPrimary, "fpAfterDataConsistentMigrationRecipientInstance", {action: "hang"});
const fpAfterForgetMigration = configureFailPoint(
    recipientPrimary, "fpAfterReceivingRecipientForgetMigration", {action: "hang"});

jsTestLog("Starting tenant migration with migrationId: " + kMigrationId +
          ", tenantId: " + kTenantId);
assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));

// Wait until a current operation corresponding to "tenant recipient migration" with state kStarted
// is visible on the recipientPrimary.
jsTestLog("Waiting until current operation with state kStarted is visible.");
fpAfterPersistingStateDoc.wait();

let res = recipientPrimary.adminCommand({currentOp: true, desc: "tenant recipient migration"});
checkStandardFieldsOK(res);
let currOp = res.inprog[0];
assert.eq(currOp.state, migrationStates.kStarted, tojson(res));
assert.eq(currOp.migrationCompleted, false, tojson(res));
assert.eq(currOp.dataSyncCompleted, false, tojson(res));
assert(!currOp.startFetchingDonorOpTime, tojson(res));
assert(!currOp.startApplyingDonorOpTime, tojson(res));
assert(!currOp.dataConsistentStopDonorOpTime, tojson(res));
assert(!currOp.cloneFinishedRecipientOpTime, tojson(res));
assert(!currOp.expireAt, tojson(res));
fpAfterPersistingStateDoc.off();

// Allow the migration to move to the point where the startFetchingDonorOpTime has been obtained.
jsTestLog("Waiting for startFetchingDonorOpTime to exist.");
fpAfterRetrievingStartOpTime.wait();

res = recipientPrimary.adminCommand({currentOp: true, desc: "tenant recipient migration"});
checkStandardFieldsOK(res);
currOp = res.inprog[0];
assert.eq(currOp.state, migrationStates.kStarted, tojson(res));
assert.eq(currOp.migrationCompleted, false, tojson(res));
assert.eq(currOp.dataSyncCompleted, false, tojson(res));
assert(!currOp.dataConsistentStopDonorOpTime, tojson(res));
assert(!currOp.cloneFinishedRecipientOpTime, tojson(res));
assert(!currOp.expireAt, tojson(res));
// Must exist now.
assert(currOp.startFetchingDonorOpTime, tojson(res));
assert(currOp.startApplyingDonorOpTime, tojson(res));
fpAfterRetrievingStartOpTime.off();

// Wait until collection cloning is done, and cloneFinishedRecipientOpTime
// and dataConsistentStopDonorOpTime are visible.
jsTestLog("Waiting for collection cloning to complete.");
fpAfterCollectionCloner.wait();

res = recipientPrimary.adminCommand({currentOp: true, desc: "tenant recipient migration"});
checkStandardFieldsOK(res);
currOp = res.inprog[0];
assert.eq(currOp.state, migrationStates.kStarted, tojson(res));
assert.eq(currOp.migrationCompleted, false, tojson(res));
assert.eq(currOp.dataSyncCompleted, false, tojson(res));
assert(!currOp.expireAt, tojson(res));
// Must exist now.
assert(currOp.startFetchingDonorOpTime, tojson(res));
assert(currOp.startApplyingDonorOpTime, tojson(res));
assert(currOp.dataConsistentStopDonorOpTime, tojson(res));
assert(currOp.cloneFinishedRecipientOpTime, tojson(res));
fpAfterCollectionCloner.off();

// Wait for the "kConsistent" state to be reached.
jsTestLog("Waiting for the kConsistent state to be reached.");
fpAfterDataConsistent.wait();

res = recipientPrimary.adminCommand({currentOp: true, desc: "tenant recipient migration"});
checkStandardFieldsOK(res);
currOp = res.inprog[0];
// State should have changed.
assert.eq(currOp.state, migrationStates.kConsistent, tojson(res));
assert.eq(currOp.migrationCompleted, false, tojson(res));
assert.eq(currOp.dataSyncCompleted, false, tojson(res));
assert(currOp.startFetchingDonorOpTime, tojson(res));
assert(currOp.startApplyingDonorOpTime, tojson(res));
assert(currOp.dataConsistentStopDonorOpTime, tojson(res));
assert(currOp.cloneFinishedRecipientOpTime, tojson(res));
assert(!currOp.expireAt, tojson(res));
fpAfterDataConsistent.off();

jsTestLog("Waiting for migration to complete.");
assert.commandWorked(tenantMigrationTest.waitForMigrationToComplete(migrationOpts));

jsTestLog("Issuing a forget migration command.");
const forgetMigrationThread =
    new Thread(TenantMigrationUtil.forgetMigrationAsync,
               migrationOpts.migrationIdString,
               TenantMigrationUtil.createRstArgs(tenantMigrationTest.getDonorRst()),
               true /* retryOnRetryableErrors */);
forgetMigrationThread.start();

jsTestLog("Waiting for the recipient to receive the forgetMigration, and pause at failpoint");
fpAfterForgetMigration.wait();

res = recipientPrimary.adminCommand({currentOp: true, desc: "tenant recipient migration"});
checkStandardFieldsOK(res);
currOp = res.inprog[0];
assert.eq(currOp.state, migrationStates.kConsistent, tojson(res));
assert.eq(currOp.migrationCompleted, false, tojson(res));
// dataSyncCompleted should have changed.
assert.eq(currOp.dataSyncCompleted, true, tojson(res));
assert(currOp.startFetchingDonorOpTime, tojson(res));
assert(currOp.startApplyingDonorOpTime, tojson(res));
assert(currOp.dataConsistentStopDonorOpTime, tojson(res));
assert(currOp.cloneFinishedRecipientOpTime, tojson(res));
assert(!currOp.expireAt, tojson(res));

jsTestLog("Allow the forgetMigration to complete.");
fpAfterForgetMigration.off();
assert.commandWorked(forgetMigrationThread.returnData());

res = recipientPrimary.adminCommand({currentOp: true, desc: "tenant recipient migration"});
checkStandardFieldsOK(res);
currOp = res.inprog[0];
assert.eq(currOp.dataSyncCompleted, true, tojson(res));
assert(currOp.startFetchingDonorOpTime, tojson(res));
assert(currOp.startApplyingDonorOpTime, tojson(res));
assert(currOp.dataConsistentStopDonorOpTime, tojson(res));
assert(currOp.cloneFinishedRecipientOpTime, tojson(res));
// State, completion status and expireAt should have changed.
assert.eq(currOp.state, migrationStates.kDone, tojson(res));
assert.eq(currOp.migrationCompleted, true, tojson(res));
assert(currOp.expireAt, tojson(res));

tenantMigrationTest.stop();
})();
