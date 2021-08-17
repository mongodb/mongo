/**
 * Tests that the collection TTL is suspended during tenant migration to
 * avoid consistency errors as the data synchronization phase may operate
 * concurrently with TTL deletions.
 *
 * @tags: [
 *   incompatible_with_eft,
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");

const garbageCollectionOpts = {
    // Set the delay before a donor state doc is garbage collected to be short to speed
    // up the test.
    tenantMigrationGarbageCollectionDelayMS: 5 * 1000,
    // Set the TTL interval large enough to decrease the probability of races.
    ttlMonitorSleepSecs: 5,
    // Allow reads on recipient before migration completes for testing.
    'failpoint.tenantMigrationRecipientNotRejectReads': tojson({mode: 'alwaysOn'}),
    // Allow non-timestamped reads on donor after migration completes for testing.
    'failpoint.tenantMigrationDonorAllowsNonTimestampedReads': tojson({mode: 'alwaysOn'}),
};

const tenantMigrationTest = new TenantMigrationTest(
    {name: jsTestName(), sharedOptions: {setParameter: garbageCollectionOpts}});

const collName = "testColl";

const donorRst = tenantMigrationTest.getDonorRst();
const recipientRst = tenantMigrationTest.getRecipientRst();
const donorPrimary = donorRst.getPrimary();
const recipientPrimary = recipientRst.getPrimary();

const numDocs = 20;

function prepareData() {
    // Timestamp to use in TTL.
    const timestamp = new Date();
    // This creates an array of tuples.
    return Array.apply(null, Array(numDocs)).map(function(x, i) {
        return {_id: i, time: timestamp};
    });
}

function prepareDb(dbName, ttlTimeoutSeconds = 0) {
    let db = donorPrimary.getDB(dbName);
    tenantMigrationTest.insertDonorDB(dbName, collName, prepareData());
    // Create TTL index.
    assert.commandWorked(
        db[collName].createIndex({time: 1}, {expireAfterSeconds: ttlTimeoutSeconds}));
}

function getNumTTLPasses(node) {
    let serverStatus = assert.commandWorked(node.adminCommand({serverStatus: 1}));
    jsTestLog(`TTL: ${tojson(serverStatus.metrics.ttl)}`);
    return serverStatus.metrics.ttl.passes;
}

function waitForOneTTLPassAtNode(node) {
    // Wait for one TTL pass.
    let initialTTLCount = getNumTTLPasses(node);
    assert.soon(() => {
        return getNumTTLPasses(node) > initialTTLCount;
    }, "TTLMonitor never did any passes.");
}

function getDocumentCount(dbName, node) {
    return node.getDB(dbName)[collName].count();
}

function assertTTLNotDeleteExpiredDocs(dbName, node) {
    assert.eq(numDocs, getDocumentCount(dbName, node));
}

function assertTTLDeleteExpiredDocs(dbName, node) {
    waitForOneTTLPassAtNode(node);
    assert.soon(() => {
        let found = getDocumentCount(dbName, node);
        jsTest.log(`${found} documents in the ${node} collection`);
        return found == 0;
    }, `TTL doesn't clean the database at ${node}`);
}

// Tests that:
// 1. At the recipient, the TTL deletions are suspended during the cloning phase.
// 2. At the donor, TTL deletions are not suspended before blocking state.
(() => {
    jsTest.log("Test that the TTL does not delete documents on recipient during cloning");

    const tenantId = "testTenantId_duringCloning";
    const dbName = tenantMigrationTest.tenantDB(tenantId, "testDB");

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: tenantId,
        recipientConnString: tenantMigrationTest.getRecipientConnString(),
    };

    // We start the test right after the donor TTL cycle.
    waitForOneTTLPassAtNode(donorPrimary);
    // The TTL timeout is intentionally shorter than TTL interval to let the documents to be subject
    // of TTL in the first round.
    prepareDb(dbName, 3);

    const recipientDb = recipientPrimary.getDB(dbName);
    let recipientColl = recipientDb.getCollection(collName);
    const hangDuringCollectionClone = configureFailPoint(
        recipientDb,
        "hangAfterClonerStage",
        {cloner: "TenantCollectionCloner", stage: "query", nss: recipientColl.getFullName()});

    assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));

    hangDuringCollectionClone.wait();

    waitForOneTTLPassAtNode(donorPrimary);
    waitForOneTTLPassAtNode(recipientPrimary);

    // All documents should expire on the donor but not on the recipient.
    assertTTLDeleteExpiredDocs(dbName, donorPrimary);
    assertTTLNotDeleteExpiredDocs(dbName, recipientPrimary);

    hangDuringCollectionClone.off();

    TenantMigrationTest.assertCommitted(
        tenantMigrationTest.waitForMigrationToComplete(migrationOpts));

    // Data should be consistent after the migration commits.
    assertTTLDeleteExpiredDocs(dbName, recipientPrimary);
    assertTTLDeleteExpiredDocs(dbName, donorPrimary);

    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
})();

// Tests that:
// 1. At the recipient, the TTL deletions are suspended after the cloning phase until migration is
//    forgotten.
// 2. At the donor, TTL deletions are suspended during blocking state. This verifies that
//    the TTL mechanism respects the same MTAB mechanism as normal updates.
(() => {
    jsTest.log("Test that the TTL does not delete documents on recipient after cloning");

    const tenantId = "testTenantId_afterCloning";
    const dbName = tenantMigrationTest.tenantDB(tenantId, "testDB");

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: tenantId,
        recipientConnString: tenantMigrationTest.getRecipientConnString(),
    };

    // We start the test right after the donor TTL cycle.
    waitForOneTTLPassAtNode(donorPrimary);
    // The TTL timeout is intentionally shorter than TTL interval to let the documents to be subject
    // of TTL in the first round. It also should be long enough to let the startMigration() finish
    // before the timeout expires.
    prepareDb(dbName, 3);

    let blockFp =
        configureFailPoint(donorPrimary, "pauseTenantMigrationBeforeLeavingBlockingState");

    assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));
    blockFp.wait();

    // At a very slow machine, there is a chance that a TTL cycle happened at the donor
    // before it entered the blocking phase. This flag is set when there was a race.
    const donorHadNoTTLCyclesBeforeBlocking = numDocs == getDocumentCount(dbName, donorPrimary);
    if (!donorHadNoTTLCyclesBeforeBlocking) {
        jsTestLog('A rare race when TTL cycle happened before donor entered its blocking phase');
        return;
    }

    // Tests that:
    // 1. TTL is suspended at the recipient
    // 2. As there was no race with TTL cycle at the donor, TTL is suspended as well.
    waitForOneTTLPassAtNode(recipientPrimary);
    assertTTLNotDeleteExpiredDocs(dbName, recipientPrimary);
    assertTTLNotDeleteExpiredDocs(dbName, donorPrimary);

    blockFp.off();

    TenantMigrationTest.assertCommitted(
        tenantMigrationTest.waitForMigrationToComplete(migrationOpts));

    // Tests that the TTL cleanup was suspended during the tenant migration.
    waitForOneTTLPassAtNode(donorPrimary);
    waitForOneTTLPassAtNode(recipientPrimary);
    assertTTLNotDeleteExpiredDocs(dbName, recipientPrimary);
    assertTTLNotDeleteExpiredDocs(dbName, donorPrimary);

    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));

    // After the tenant migration is aborted, the TTL cleanup is restored.
    assertTTLDeleteExpiredDocs(dbName, recipientPrimary);
    assertTTLDeleteExpiredDocs(dbName, donorPrimary);
})();

tenantMigrationTest.stop();
})();
