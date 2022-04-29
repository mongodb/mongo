/**
 * Tests that the collection TTL is suspended during tenant migration to
 * avoid consistency errors as the data synchronization phase may operate
 * concurrently with TTL deletions.
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
    jsTestLog(`TTL ${node}: ${tojson(serverStatus.metrics.ttl)}`);
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
    // Use "countDocuments" instead of "count" to ensure that we get an accurate
    // count instead of an approximate count from metadata. Otherwise, the count
    // can be inaccurate if a TTL pass happens concurrently with the count call when
    // the access blocker is blocking writes. In this case, the TTL delete will fail and
    // be rolled back, but count calls before the rollback is applied will still reflect
    // the delete.
    return node.getDB(dbName)[collName].countDocuments({});
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
    if (TenantMigrationUtil.isShardMergeEnabled(donorPrimary.getDB("admin"))) {
        jsTestLog(
            "Skip: featureFlagShardMerge enabled, but shard merge does not use logical cloning");
        return;
    }

    jsTest.log("Test that the TTL does not delete documents on recipient during cloning");

    const tenantId = "testTenantId-duringCloning";
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
    const hangBeforeFetchingCommittedTransactions =
        configureFailPoint(recipientDb, "fpBeforeFetchingCommittedTransactions", {action: "hang"});
    assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));

    hangBeforeFetchingCommittedTransactions.wait();

    // On a very slow machine, there is a chance that a TTL cycle happened at the donor before the
    // recipient cloned the documents. Therefore, these checks are only valid when we are sure the
    // TTL cycle hasn't occurred yet on the donor.
    if (getDocumentCount(dbName, donorPrimary) == numDocs) {
        waitForOneTTLPassAtNode(donorPrimary);
        waitForOneTTLPassAtNode(recipientPrimary);

        // All documents should expire on the donor but not on the recipient.
        assertTTLDeleteExpiredDocs(dbName, donorPrimary);
        assertTTLNotDeleteExpiredDocs(dbName, recipientPrimary);
    }

    hangBeforeFetchingCommittedTransactions.off();

    TenantMigrationTest.assertCommitted(
        tenantMigrationTest.waitForMigrationToComplete(migrationOpts));

    // Data should be consistent after the migration commits.
    assertTTLDeleteExpiredDocs(dbName, recipientPrimary);
    assertTTLDeleteExpiredDocs(dbName, donorPrimary);

    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
    tenantMigrationTest.waitForMigrationGarbageCollection(migrationOpts.migrationIdString,
                                                          migrationOpts.tenantId);
})();

// Tests that:
// 1. At the recipient, the TTL deletions are suspended until migration is forgotten.
// 2. At the donor, TTL deletions are suspended during blocking state. This verifies that
//    the TTL mechanism respects the same MTAB mechanism as normal updates.
(() => {
    jsTest.log(
        "Test that the TTL does not delete documents on recipient before migration is forgotten");

    const tenantId = "testTenantId-afterCloning";
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

    assert.commandWorked(
        tenantMigrationTest.startMigration(migrationOpts, {enableDonorStartMigrationFsync: true}));
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

    tenantMigrationTest.waitForMigrationGarbageCollection(migrationOpts.migrationIdString,
                                                          migrationOpts.tenantId);
})();

tenantMigrationTest.stop();
})();
