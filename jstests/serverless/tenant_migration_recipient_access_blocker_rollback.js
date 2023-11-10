/**
 * Tests if the recipient is rolled back well after a migration has been committed, the tenant
 * migration recipient access blocker is initialized in the correct state.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   # Shard merge protocol will be tested by
 *   # tenant_migration_shard_merge_recipient_access_blocker_rollback.js.
 *   incompatible_with_shard_merge,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 *   requires_fcv_71,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {restartServerReplication, stopServerReplication} from "jstests/libs/write_concern_util.js";
import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";
import {isShardMergeEnabled, makeTenantDB} from "jstests/replsets/libs/tenant_migration_util.js";

const recipientRst = new ReplSetTest({
    name: "recipRst",
    nodes: 3,
    serverless: true,
    settings: {catchUpTimeoutMillis: 0, chainingAllowed: false}
});

recipientRst.startSet();
recipientRst.initiate();

if (isShardMergeEnabled(recipientRst.getPrimary().getDB("admin"))) {
    recipientRst.stopSet();
    jsTestLog("Skipping this shard merge incompatible test.");
    quit();
}

// This test case
// 1) Completes and commits a tenant migration. Then forgets the migration (state doc marked with
//    'expireAt', but not yet deleted.)
// 2) Waits until the replica set is stable.
// 3) Rolls back the primary. This makes the primary recover its tenant migration access blockers.
// 4) Ensures that a read is possible from the primary.
function runRollbackAfterMigrationCommitted(tenantId) {
    jsTestLog("Testing a rollback after the migration has been committed and marked forgotten.");
    const tenantMigrationTest = new TenantMigrationTest(
        {name: jsTestName(), recipientRst: recipientRst, sharedOptions: {nodes: 1}});

    const kMigrationId = UUID();
    const kTenantId = tenantId;
    const kReadPreference = {mode: "primary"};
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(kMigrationId),
        tenantId: kTenantId,
        readPreference: kReadPreference
    };

    // Populate the donor side with data.
    const dbName = makeTenantDB(kTenantId, "testDB");
    const collName = "testColl";
    const numDocs = 20;
    tenantMigrationTest.insertDonorDB(
        dbName,
        collName,
        [...Array(numDocs).keys()].map((i) => ({a: i, band: "Air", song: "La Femme d'Argent"})));

    jsTestLog(`Starting tenant migration with migrationId ${kMigrationId}, tenantId: ${kTenantId}`);
    assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));

    // Complete and commit the migration, and then forget it as well.
    jsTestLog("Waiting for migration to complete and commit.");
    TenantMigrationTest.assertCommitted(
        tenantMigrationTest.waitForMigrationToComplete(migrationOpts));
    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
    recipientRst.awaitReplication();

    // It should be possible to read from the recipient now.
    jsTestLog("Reading from the recipient primary on the tenant collection.");
    const originalPrimary = recipientRst.getPrimary();
    assert.eq(numDocs, originalPrimary.getDB(dbName)[collName].find().itcount());

    jsTestLog("Halting replication on the secondaries.");
    const secondaries = recipientRst.getSecondaries();
    stopServerReplication(secondaries);

    // Prepare the recipient primary for rollback, by inserting non-tenant related data on it while
    // replication has been halted.
    jsTestLog("Inserting random data on recipient primary.");
    const randomColl = originalPrimary.getDB("randomDB")["random_coll"];
    assert.commandWorked(randomColl.insert({x: "The Real Folk Blues"}, {writeConcern: {w: 1}}));

    // Stepping up one of the secondaries should cause the original primary to rollback.
    jsTestLog("Stepping up one of the secondaries.");
    const newRecipientPrimary = secondaries[0];
    recipientRst.stepUp(newRecipientPrimary, {awaitReplicationBeforeStepUp: false});

    jsTestLog("Restarting server replication.");
    restartServerReplication(secondaries);
    recipientRst.awaitReplication();

    jsTestLog("Stepping up the original primary back to primary.");
    recipientRst.stepUp(originalPrimary, {awaitReplicationBeforeStepUp: false});

    jsTestLog("Perform a read against the original primary on the tenant collection.");
    assert.eq(numDocs, originalPrimary.getDB(dbName)[collName].find().itcount());

    tenantMigrationTest.stop();
}

// This test case:
// 1) Sets the replica set up such that the migration has already been committed and forgotten, and
//    the state doc has been deleted as well.
// 2) Sends a 'recipientForgetMigration' command to the recipient primary, and waits for the state
//    doc to persist.
// 3) Performs a rollback on the recipient primary, so that the access blockers are reconstructed.
// 4) Performs a read on the recipient primary.
function runRollbackAfterLoneRecipientForgetMigrationCommand(tenantId) {
    jsTestLog("Testing a rollback after migration has been committed and completely forgotten.");
    const tenantMigrationTest = new TenantMigrationTest(
        {name: jsTestName(), recipientRst: recipientRst, sharedOptions: {nodes: 1}});

    const kMigrationId = UUID();
    const kTenantId = tenantId;
    const kReadPreference = {mode: "primary"};

    const dbName = makeTenantDB(kTenantId, "testDB");
    const collName = "testColl";

    const originalPrimary = recipientRst.getPrimary();
    const newPrimary = recipientRst.getSecondaries()[0];

    // We will have the tenant database already on the recipient, as though the tenant migration has
    // already run to completion, and the state document has been cleaned up already.
    assert.commandWorked(originalPrimary.getDB(dbName)[collName].insert(
        {x: "Composer", y: "Mysore Vasudevacharya"}));
    recipientRst.awaitReplication();

    // Prevent the "expireAt" field from being populated.
    const fpOriginalPrimary = configureFailPoint(originalPrimary, "hangBeforeTaskCompletion");
    // Prevent the new primary from marking the state document as garbage collectable.
    const fpNewPrimary =
        configureFailPoint(newPrimary, "pauseBeforeRunTenantMigrationRecipientInstance");

    function runRecipientForgetMigration(
        host, {migrationIdString, donorConnectionString, tenantId, readPreference}) {
        const db = new Mongo(host);
        const res = executeNoThrowNetworkError(() => db.adminCommand({
            recipientForgetMigration: 1,
            migrationId: UUID(migrationIdString),
            donorConnectionString,
            tenantId,
            readPreference
        }));

        // We need to handle network errors as this thread might race with rollback.
        assert.commandFailedWithCode(
            res,
            [ErrorCodes.InterruptedDueToReplStateChange, ErrorCodes.HostUnreachable],
            tojson(res.code));
    }

    const recipientForgetMigrationThread =
        new Thread(runRecipientForgetMigration, originalPrimary.host, {
            migrationIdString: extractUUIDFromObject(kMigrationId),
            donorConnectionString: tenantMigrationTest.getDonorRst().getURL(),
            tenantId: kTenantId,
            readPreference: kReadPreference
        });

    // Run a delayed/retried recipientForgetMigration command after the state doc has been deleted.
    recipientForgetMigrationThread.start();

    jsTestLog("Wait until the right before the state document's 'expireAt' is set.");
    fpOriginalPrimary.wait();
    recipientRst.awaitReplication();

    // It should be possible to read from the recipient now.
    assert.eq(1, originalPrimary.getDB(dbName)[collName].find().itcount());

    // Now perform a rollback on the recipient primary.
    jsTestLog("Halting replication on the secondaries.");
    const secondaries = recipientRst.getSecondaries();
    stopServerReplication(secondaries);

    jsTestLog("Inserting random data on recipient primary.");
    const randomColl = originalPrimary.getDB("randomDB")["random_coll"];
    assert.commandWorked(randomColl.insert({x: "Que Sera Sera"}, {writeConcern: {w: 1}}));

    // Stepping up one of the secondaries should cause the original primary to rollback.
    jsTestLog("Stepping up one of the secondaries.");
    recipientRst.stepUp(newPrimary, {awaitReplicationBeforeStepUp: false});

    recipientForgetMigrationThread.join();

    // It should be possible to read from new recipient primary.
    assert.eq(1, newPrimary.getDB(dbName)[collName].find().itcount());

    jsTestLog("Restarting server replication.");
    restartServerReplication(secondaries);
    recipientRst.awaitReplication();

    jsTestLog("Stepping up the original primary back to primary.");
    const fpOriginalPrimaryBeforeStarting =
        configureFailPoint(originalPrimary, "pauseBeforeRunTenantMigrationRecipientInstance");
    fpOriginalPrimary.off();
    recipientRst.stepUp(originalPrimary, {awaitReplicationBeforeStepUp: false});

    jsTestLog("Perform another read against the original primary on the tenant collection.");
    assert.eq(1, originalPrimary.getDB(dbName)[collName].find().itcount());

    fpOriginalPrimaryBeforeStarting.off();
    fpNewPrimary.off();

    tenantMigrationTest.stop();
}

runRollbackAfterMigrationCommitted(ObjectId().str);
runRollbackAfterLoneRecipientForgetMigrationCommand(ObjectId().str);

recipientRst.stopSet();
