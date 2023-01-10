/**
 * Tests tenant migration cloner stats such as 'approxTotalDataSize', 'approxTotalBytesCopied'
 * across multiple databases and collections in the absence of failovers.
 *
 * TODO SERVER-63517: incompatible_with_shard_merge because this specifically tests logical
 * cloning behavior.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   incompatible_with_shard_merge,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 * ]
 */

(function() {
"use strict";
load("jstests/libs/uuid_util.js");        // For extractUUIDFromObject().
load("jstests/libs/fail_point_util.js");  // For configureFailPoint().
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");

// Limit the batch size to test the stat in between batches.
const tenantMigrationTest = new TenantMigrationTest(
    {name: jsTestName(), sharedOptions: {setParameter: {collectionClonerBatchSize: 10}}});

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

const dbName = tenantMigrationTest.tenantDB(kTenantId, "testDB");
const collName = "testColl";

const dbName1 = dbName + '_db_1';
const dbName2 = dbName + '_db_2';
const collName1 = collName + "_coll_1";
const collName2 = collName + "_coll_2";
const collNameDb2 = collName + "_only_coll";

const dataForEachCollection = [...Array(100).keys()].map((i) => ({a: i, b: 'A very long string.'}));
tenantMigrationTest.insertDonorDB(dbName1, collName1, dataForEachCollection);
tenantMigrationTest.insertDonorDB(dbName1, collName2, dataForEachCollection);
tenantMigrationTest.insertDonorDB(dbName2, collNameDb2, dataForEachCollection);

// Assert that the number of databases and collections cloned before failover is 0, as no failovers
// occur during this test.
function assertNothingClonedBeforeFailover(currOpResult) {
    const currOp = currOpResult.inprog[0];
    const dbInfo = currOp.databases;
    assert.eq(dbInfo.databasesClonedBeforeFailover, 0, currOpResult);
    assert.eq(dbInfo[dbName1].clonedCollectionsBeforeFailover, 0, currOpResult);
    assert.eq(dbInfo[dbName2].clonedCollectionsBeforeFailover, 0, currOpResult);
}

jsTestLog("Set up fail points on recipient.");
const recipientPrimary = tenantMigrationTest.getRecipientPrimary();
const fpAfterPersistingStateDoc =
    configureFailPoint(recipientPrimary,
                       "fpAfterPersistingTenantMigrationRecipientInstanceStateDoc",
                       {action: "hang"});
const fpAfterCreateFirstCollection = configureFailPoint(
    recipientPrimary, "tenantCollectionClonerHangAfterCreateCollection", {action: "hang"});

const donorPrimary = tenantMigrationTest.getDonorPrimary();
const donorDB1 = donorPrimary.getDB(dbName1);

const db1Size = assert.commandWorked(donorDB1.runCommand({dbStats: 1})).dataSize;
const db2Size = assert.commandWorked(donorPrimary.getDB(dbName2).runCommand({dbStats: 1})).dataSize;

const db1Collection1Size = assert.commandWorked(donorDB1.runCommand({collStats: collName1})).size;
const db1Collection2Size = assert.commandWorked(donorDB1.runCommand({collStats: collName2})).size;

const donorStats = {
    db1Size,
    db2Size,
    db1Collection1Size,
    db1Collection2Size
};

jsTestLog("Collected the following stats on the donor: " + tojson(donorStats));

jsTestLog("Starting tenant migration with migrationId: " + kMigrationId +
          ", tenantId: " + kTenantId);
assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));

// In this case, we do not expect the stats to exist yet, as the cloner has not been started.
jsTestLog("Waiting until the state doc has been persisted.");
fpAfterPersistingStateDoc.wait();
let res = recipientPrimary.adminCommand({currentOp: true, desc: "tenant recipient migration"});
let currOp = res.inprog[0];
assert(!currOp.hasOwnProperty("approxTotalDataSize"), res);
assert(!currOp.hasOwnProperty("approxTotalBytesCopied"), res);
assert(!currOp.hasOwnProperty("totalReceiveElapsedMillis"), res);
assert(!currOp.hasOwnProperty("remainingReceiveEstimatedMillis"), res);
assert(!currOp.hasOwnProperty("databases"), res);
fpAfterPersistingStateDoc.off();

// At this point, the total data size stat will have been obtained. However, nothing has been
// copied yet.
jsTestLog("Wait until the cloner has created the first collection");
fpAfterCreateFirstCollection.wait();
res = recipientPrimary.adminCommand({currentOp: true, desc: "tenant recipient migration"});
currOp = res.inprog[0];
assert.eq(currOp.approxTotalDataSize, db1Size + db2Size, res);
assert.eq(currOp.approxTotalBytesCopied, 0, res);
assert.gt(currOp.totalReceiveElapsedMillis, 0, res);
assert.gt(currOp.remainingReceiveEstimatedMillis, 0, res);
assertNothingClonedBeforeFailover(res);

// Before proceeding, set the failpoint to pause after cloning a single batch.
jsTestLog("Setting failpoint to pause after cloning single batch.");
const fpAfterFirstBatch = configureFailPoint(
    recipientPrimary, "tenantMigrationHangCollectionClonerAfterHandlingBatchResponse");
fpAfterCreateFirstCollection.off();

// After copying one batch, the amount of data copied should be non-zero, but less than the size
// of the collection.
jsTestLog("Waiting for a single batch of documents to have been cloned.");
fpAfterFirstBatch.wait();

// Since documents are inserted on a separate thread, wait until the expected stats are seen. The
// failpoint needs to be maintained so that the next batch isn't processed.
assert.soon(() => {
    res = recipientPrimary.adminCommand({currentOp: true, desc: "tenant recipient migration"});
    currOp = res.inprog[0];

    // Wait until one batch of documents has been copied.
    return currOp.approxTotalBytesCopied > 0;
}, res);

assert.eq(currOp.approxTotalDataSize, db1Size + db2Size, res);
assert.gt(currOp.approxTotalBytesCopied, 0, res);
assert.lt(currOp.approxTotalBytesCopied, db1Collection1Size, res);
assert.gt(currOp.totalReceiveElapsedMillis, 0, res);
assertNothingClonedBeforeFailover(res);
// At this point, most of the data is un-cloned.
assert.gt(currOp.remainingReceiveEstimatedMillis, currOp.totalReceiveElapsedMillis, res);

// Before proceeding, set fail point to pause at the next create collection boundary.
const fpAfterCreateSecondCollection = configureFailPoint(
    recipientPrimary, "tenantCollectionClonerHangAfterCreateCollection", {action: "hang"});
fpAfterFirstBatch.off();

// One collection should have been cloned completely. The stats should reflect this.
jsTestLog("Waiting for the second collection to be created.");
fpAfterCreateSecondCollection.wait();
res = recipientPrimary.adminCommand({currentOp: true, desc: "tenant recipient migration"});
currOp = res.inprog[0];
assert.eq(currOp.approxTotalDataSize, db1Size + db2Size, res);
assert.eq(currOp.approxTotalBytesCopied, db1Collection1Size, res);
assert.gt(currOp.totalReceiveElapsedMillis, 0, res);
assert.gt(currOp.remainingReceiveEstimatedMillis, currOp.totalReceiveElapsedMillis, res);
assertNothingClonedBeforeFailover(res);
let prevTotalElapsedMillis = currOp.totalReceiveElapsedMillis;
const prevRemainingMillis = currOp.remainingReceiveEstimatedMillis;

// Before proceeding, set fail point to pause before copying the second database.
const fpBeforeCopyingSecondDB =
    configureFailPoint(recipientPrimary, "tenantDatabaseClonerHangAfterGettingOperationTime");
fpAfterCreateSecondCollection.off();

jsTestLog("Wait until the second database is about to be cloned.");
fpBeforeCopyingSecondDB.wait();
res = recipientPrimary.adminCommand({currentOp: true, desc: "tenant recipient migration"});
currOp = res.inprog[0];
assert.eq(currOp.approxTotalDataSize, db1Size + db2Size, res);
assert.eq(currOp.approxTotalBytesCopied, db1Size, res);
assert.gt(currOp.totalReceiveElapsedMillis, prevTotalElapsedMillis, res);
assertNothingClonedBeforeFailover(res);
// We have copied most of the data.
assert.lt(currOp.remainingReceiveEstimatedMillis, currOp.totalReceiveElapsedMillis, res);
assert.lt(currOp.remainingReceiveEstimatedMillis, prevRemainingMillis);
prevTotalElapsedMillis = currOp.totalReceiveElapsedMillis;
fpBeforeCopyingSecondDB.off();

// After the migration completes, the total bytes copied should be equal to the total data size.
jsTestLog("Waiting for migration to complete.");
TenantMigrationTest.assertCommitted(tenantMigrationTest.waitForMigrationToComplete(migrationOpts));
res = recipientPrimary.adminCommand({currentOp: true, desc: "tenant recipient migration"});
currOp = res.inprog[0];
assert.eq(currOp.approxTotalDataSize, db1Size + db2Size, res);
assert.eq(currOp.approxTotalBytesCopied, db1Size + db2Size, res);
assert.gt(currOp.totalReceiveElapsedMillis, prevTotalElapsedMillis, res);
assertNothingClonedBeforeFailover(res);
// We have finished cloning, therefore time remaining is zero.
assert.eq(currOp.remainingReceiveEstimatedMillis, 0, res);

tenantMigrationTest.stop();
})();
