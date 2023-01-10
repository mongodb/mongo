/**
 * Tests tenant migration cloner stats such as 'approxTotalDataSize', 'approxTotalBytesCopied',
 * 'databasesClonedBeforeFailover' across multiple databases and collections with failovers.
 *
 * This test does the following:
 * 1. Insert two databases on the donor. The first database consists of one collection, the second
 *    consists of two collections.
 * 2. Wait for the primary (referred to as the original primary) to clone one batch from the second
 *    database's second collection.
 * 3. Step up the new primary. Ensure that the stats such as 'databasesClonedBeforeFailover' tally.
 * 4. Allow the tenant migration to complete and commit. Ensure that stats are sensible.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_shard_merge,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 *   incompatible_with_shard_merge,
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
const collName = "coll";
const dbName1 = dbName + '_db_1';
const dbName2 = dbName + '_db_2';
const db2Coll1 = collName + "_db_2_1";
const db2Coll2 = collName + "_db_2_2";

// Add a large amount of data to the donor.
jsTestLog("Adding data to donor.");
const dataForEachCollection = [...Array(100).keys()].map((i) => ({a: i, b: 'metanoia'}));
tenantMigrationTest.insertDonorDB(dbName1, collName + "_1", dataForEachCollection);
tenantMigrationTest.insertDonorDB(dbName2, db2Coll1, dataForEachCollection);
tenantMigrationTest.insertDonorDB(dbName2, db2Coll2, dataForEachCollection);

const originalRecipientPrimary = tenantMigrationTest.getRecipientPrimary();
const newRecipientPrimary = tenantMigrationTest.getRecipientRst().getSecondaries()[0];

jsTestLog("Collecting the stats of the databases and collections from the donor.");
const donorPrimary = tenantMigrationTest.getDonorPrimary();
const donorDB2 = donorPrimary.getDB(dbName2);

const db1Size = assert.commandWorked(donorPrimary.getDB(dbName1).runCommand({dbStats: 1})).dataSize;
const db2Size = assert.commandWorked(donorDB2.runCommand({dbStats: 1})).dataSize;
const db2Collection1Size = assert.commandWorked(donorDB2.runCommand({collStats: db2Coll1})).size;
const db2Collection2Size = assert.commandWorked(donorDB2.runCommand({collStats: db2Coll2})).size;

const donorStats = {
    db1Size,
    db2Size,
    db2Collection1Size,
    db2Collection2Size
};
jsTestLog("Collected the following stats on the donor: " + tojson(donorStats));

// The last collection to be cloned is the one with a greater UUID.
const collInfo = donorDB2.getCollectionInfos();
const uuid1 = collInfo[0].info.uuid;
const uuid2 = collInfo[1].info.uuid;
const lastCollection = (uuid1 > uuid2) ? db2Coll1 : db2Coll2;

// Create a failpoint to pause after one batch of the second database's second collection has been
// cloned.
const fpAfterBatchOfSecondDB = configureFailPoint(
    originalRecipientPrimary,
    "tenantMigrationHangCollectionClonerAfterHandlingBatchResponse",
    {nss: originalRecipientPrimary.getDB(dbName2).getCollection(lastCollection).getFullName()});

jsTestLog("Starting tenant migration with migrationId: " + kMigrationId +
          ", tenantId: " + kTenantId);
assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));

let res = 0;
let currOp = 0;
jsTestLog("Waiting until one batch of second database has been cloned by original primary.");
fpAfterBatchOfSecondDB.wait();
// Since documents are inserted on a separate thread, wait until the expected stats are seen. The
// failpoint needs to be maintained so that the next batch isn't processed.
assert.soon(() => {
    res = originalRecipientPrimary.adminCommand(
        {currentOp: true, desc: "tenant recipient migration"});
    currOp = res.inprog[0];

    // Wait until one batch of documents of the second database's second collection has been copied.
    return currOp.approxTotalBytesCopied > db1Size + db2Collection1Size;
}, res);

assert.eq(currOp.approxTotalDataSize, db1Size + db2Size, res);
// Since the two collections on the second database are the same size,
// 'db1Size + db2Collection1Size' and 'db1Size + db2Collection2Size' evaluate to the same value.
assert.gt(currOp.approxTotalBytesCopied, db1Size + db2Collection1Size, res);
assert.lt(currOp.approxTotalBytesCopied, db1Size + db2Size, res);
assert.eq(currOp.databases.databasesClonedBeforeFailover, 0, res);
assert.eq(currOp.databases[dbName2].clonedCollectionsBeforeFailover, 0, res);
const bytesCopiedIncludingSecondDB = currOp.approxTotalBytesCopied;
jsTestLog("Bytes copied after first batch of second database: " + bytesCopiedIncludingSecondDB);

// Wait until the batch of the second collection of the second database has been replicated from the
// original primary to the new primary. Then, step up the new primary.
const fpAfterCreatingCollectionOfSecondDB =
    configureFailPoint(newRecipientPrimary, "tenantCollectionClonerHangAfterCreateCollection");
tenantMigrationTest.getRecipientRst().stepUp(newRecipientPrimary);
fpAfterBatchOfSecondDB.off();

jsTestLog("Wait until the new primary creates collection of second database.");
fpAfterCreatingCollectionOfSecondDB.wait();
res = newRecipientPrimary.adminCommand({currentOp: true, desc: "tenant recipient migration"});
currOp = res.inprog[0];
assert.eq(currOp.approxTotalDataSize, db1Size + db2Size, res);
assert.eq(currOp.approxTotalBytesCopied, bytesCopiedIncludingSecondDB, res);
assert.eq(currOp.databases.databasesClonedBeforeFailover, 1, res);
assert.eq(currOp.databases[dbName2].clonedCollectionsBeforeFailover, 1, res);
fpAfterCreatingCollectionOfSecondDB.off();

// After the migration completes, the total bytes copied should be equal to the total data size.
jsTestLog("Waiting for migration to complete.");
TenantMigrationTest.assertCommitted(tenantMigrationTest.waitForMigrationToComplete(migrationOpts));
res = newRecipientPrimary.adminCommand({currentOp: true, desc: "tenant recipient migration"});
currOp = res.inprog[0];
assert.eq(currOp.approxTotalDataSize, db1Size + db2Size, res);
assert.eq(currOp.approxTotalBytesCopied, db1Size + db2Size, res);
assert.eq(currOp.databases.databasesClonedBeforeFailover, 1, res);
assert.eq(currOp.databases[dbName2].clonedCollectionsBeforeFailover, 1, res);

tenantMigrationTest.stop();
})();
