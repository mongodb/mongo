/**
 * Tests that a migration will retry if the oplog fetcher discovers that its sync source is too
 * stale. We test this with a donor replica set that has two secondaries, 'donorSecondary' and
 * 'delayedSecondary'. We force the recipient to sync from 'donorSecondary'. Then, after the
 * recipient has set its 'startFetchingDonorOpTime', we stop replication on 'delayedSecondary' and
 * advance the OpTime on 'donorSecondary'. Next, we wait until the recipient is about to report that
 * it has reached a consistent state. At this point, it should have advanced its 'lastFetched' to be
 * ahead of 'startFetchingDonorOpTime'. After forcing the recipient to restart and sync from
 * 'delayedSecondary', it should see that it is too stale. As a result, it should retry sync source
 * selection until it finds a sync source that is no longer too stale.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   # Shard merge can only sync from primary therefore this test is not applicable.
 *   incompatible_with_shard_merge,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   # The currentOp output field 'dataSyncCompleted' was renamed to 'migrationCompleted'.
 *   requires_fcv_70,
 *   serverless,
 * ]
 */

import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";
import {makeX509OptionsForTest} from "jstests/replsets/libs/tenant_migration_util.js";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");
load("jstests/libs/write_concern_util.js");
load('jstests/replsets/rslib.js');

const donorRst = new ReplSetTest({
    name: `${jsTestName()}_donor`,
    nodes: 3,
    serverless: true,
    settings: {chainingAllowed: false},
    nodeOptions: Object.assign(makeX509OptionsForTest().donor, {
        setParameter: {
            // Allow non-timestamped reads on donor after migration completes for testing.
            'failpoint.tenantMigrationDonorAllowsNonTimestampedReads': tojson({mode: 'alwaysOn'}),
        }
    }),
});
donorRst.startSet();
donorRst.initiateWithHighElectionTimeout();

const tenantMigrationTest = new TenantMigrationTest({
    name: jsTestName(),
    donorRst,
    // Set a low value for excluding donor hosts so that the test doesn't take long to retry a sync
    // source.
    sharedOptions: {setParameter: {tenantMigrationExcludeDonorHostTimeoutMS: 1000}}
});

const tenantId = ObjectId().str;
const tenantDB = tenantMigrationTest.tenantDB(tenantId, "testDB");
const collName = "testColl";

const delayedSecondary = donorRst.getSecondaries()[0];
const donorSecondary = donorRst.getSecondaries()[1];

const recipientPrimary = tenantMigrationTest.getRecipientPrimary();
const hangRecipientPrimaryAfterCreatingRSM =
    configureFailPoint(recipientPrimary, 'hangAfterCreatingRSM');
const hangRecipientPrimaryAfterRetrievingStartOpTimes = configureFailPoint(
    recipientPrimary, 'fpAfterRetrievingStartOpTimesMigrationRecipientInstance', {action: "hang"});
const hangRecipientPrimaryBeforeConsistentState = configureFailPoint(
    recipientPrimary, 'fpBeforeFulfillingDataConsistentPromise', {action: "hang"});

const migrationId = UUID();
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(migrationId),
    tenantId,
    // Configure the recipient primary to only choose a secondary as sync source.
    readPreference: {mode: 'secondary'}
};

jsTestLog("Starting the tenant migration");
assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));
hangRecipientPrimaryAfterCreatingRSM.wait();

awaitRSClientHosts(recipientPrimary, donorSecondary, {ok: true, secondary: true});
awaitRSClientHosts(recipientPrimary, delayedSecondary, {ok: true, secondary: true});

// Turn on the 'waitInHello' failpoint. This will cause the delayed secondary to cease sending
// hello responses and the RSM should mark the node as down. This is necessary so that the
// delayed secondary is not chosen as the sync source here.
jsTestLog(
    "Turning on waitInHello failpoint. Delayed donor secondary should stop sending hello responses.");
const helloFailpoint = configureFailPoint(delayedSecondary, "waitInHello");
awaitRSClientHosts(recipientPrimary, delayedSecondary, {ok: false});

hangRecipientPrimaryAfterCreatingRSM.off();
hangRecipientPrimaryAfterRetrievingStartOpTimes.wait();

let res = recipientPrimary.adminCommand({currentOp: true, desc: "tenant recipient migration"});
let currOp = res.inprog[0];
// The migration should not be complete.
assert.eq(currOp.garbageCollectable, false, tojson(res));
assert.eq(currOp.migrationCompleted, false, tojson(res));
// The sync source can only be 'donorSecondary'.
assert.eq(donorSecondary.host, currOp.donorSyncSource, tojson(res));

helloFailpoint.off();

// Stop replicating on one of the secondaries and advance the OpTime on the other nodes in the
// donor replica set.
jsTestLog("Stopping replication on delayedSecondary");
stopServerReplication(delayedSecondary);
tenantMigrationTest.insertDonorDB(tenantDB, collName);

// Wait for 'lastFetched' to be advanced on the recipient.
hangRecipientPrimaryAfterRetrievingStartOpTimes.off();
hangRecipientPrimaryBeforeConsistentState.wait();

const hangAfterRetrievingOpTimesAfterRestart = configureFailPoint(
    recipientPrimary, 'fpAfterRetrievingStartOpTimesMigrationRecipientInstance', {action: "hang"});

jsTestLog("Stopping donorSecondary");
donorRst.stop(donorSecondary);
awaitRSClientHosts(recipientPrimary, delayedSecondary, {ok: true, secondary: true});
awaitRSClientHosts(recipientPrimary, donorSecondary, {ok: false});

hangRecipientPrimaryBeforeConsistentState.off();
const configRecipientNs = recipientPrimary.getCollection(TenantMigrationTest.kConfigRecipientsNS);
assert.soon(() => {
    // Wait for the recipient to realize that the donor sync source has been shut down and retry
    // sync source selection.
    const recipientDoc = configRecipientNs.find({"_id": migrationId}).toArray();
    return recipientDoc[0].numRestartsDueToDonorConnectionFailure == 1;
});

hangAfterRetrievingOpTimesAfterRestart.wait();

res = recipientPrimary.adminCommand({currentOp: true, desc: "tenant recipient migration"});
currOp = res.inprog[0];
// The migration should not be complete.
assert.eq(currOp.garbageCollectable, false, tojson(res));
assert.eq(currOp.migrationCompleted, false, tojson(res));
// Since 'donorSecondary' was shut down, the sync source can only be 'delayedSecondary'.
assert.eq(delayedSecondary.host, currOp.donorSyncSource, tojson(res));

const hangAfterPersistingTenantMigrationRecipientInstanceStateDoc =
    configureFailPoint(recipientPrimary,
                       "fpAfterPersistingTenantMigrationRecipientInstanceStateDoc",
                       {action: "hang"});
hangAfterRetrievingOpTimesAfterRestart.off();

assert.soon(() => {
    // Verify that the recipient eventually has to restart again, since its lastFetched is ahead of
    // the last OpTime on 'delayedSecondary'.
    const recipientDoc = configRecipientNs.find({"_id": migrationId}).toArray();
    return recipientDoc[0].numRestartsDueToDonorConnectionFailure >= 2;
});

hangAfterPersistingTenantMigrationRecipientInstanceStateDoc.wait();

// Let 'delayedSecondary' catch back up to the recipient's lastFetched OpTime.
donorRst.remove(donorSecondary);
restartServerReplication(delayedSecondary);
donorRst.awaitReplication();
hangAfterPersistingTenantMigrationRecipientInstanceStateDoc.off();

// Verify that the migration eventually commits successfully.
TenantMigrationTest.assertCommitted(tenantMigrationTest.waitForMigrationToComplete(migrationOpts));

donorRst.stopSet();
tenantMigrationTest.stop();
