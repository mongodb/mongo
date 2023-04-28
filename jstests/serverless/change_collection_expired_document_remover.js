/**
 * Tests the change collection periodic remover job.
 *
 * @tags: [requires_fcv_62]
 */

(function() {
"use strict";

// For configureFailPoint.
load("jstests/libs/fail_point_util.js");
// For assertDropAndRecreateCollection.
load("jstests/libs/collection_drop_recreate.js");
// For ChangeStreamMultitenantReplicaSetTest.
load("jstests/serverless/libs/change_collection_util.js");
// For FeatureFlagUtil.
load("jstests/libs/feature_flag_util.js");

const getTenantConnection = ChangeStreamMultitenantReplicaSetTest.getTenantConnection;

// Sleep interval in seconds for the change collection remover job.
const kExpiredRemovalJobSleepSeconds = 5;
// Number of seconds after which the documents in change collections will be expired.
const kExpireAfterSeconds = 1;
// Number of seconds to sleep before inserting the next batch of documents in collections.
const kSleepBetweenWritesSeconds = 5;
// Millisecond(s) that can be added to the wall time to advance it marginally.
const kSafetyMarginMillis = 1;
// To imitate 1-by-1 deletion we specify a low amount of bytes per marker.
const kMinBytesPerMarker = 1;

const replSet = new ChangeStreamMultitenantReplicaSetTest({
    nodes: 2,
    nodeOptions: {
        setParameter: {
            changeCollectionTruncateMarkersMinBytes: kMinBytesPerMarker,
            changeCollectionExpiredDocumentsRemoverJobSleepSeconds: kExpiredRemovalJobSleepSeconds
        }
    }
});

const primary = replSet.getPrimary();
const secondary = replSet.getSecondary();

// Assert that the change collection contains all documents in 'expectedRetainedDocs' and no
// document in 'expectedDeletedDocs' for the collection 'stocksColl'.
function assertChangeCollectionDocuments(
    changeColl, stocksColl, expectedDeletedDocs, expectedRetainedDocs) {
    const collNss = `${stocksTestDb.getName()}.${stocksColl.getName()}`;
    const pipeline = (collectionEntries) => [{$match: {op: "i", ns: collNss}},
                                             {$replaceRoot: {"newRoot": "$o"}},
                                             {$match: {$or: collectionEntries}}];

    // Assert that querying for 'expectedRetainedDocs' yields documents that are exactly the same as
    // 'expectedRetainedDocs'.
    if (expectedRetainedDocs.length > 0) {
        assert.soonNoExcept(() => {
            const retainedDocs = changeColl.aggregate(pipeline(expectedRetainedDocs)).toArray();
            assert.eq(retainedDocs, expectedRetainedDocs);
            return true;
        });
    }

    // Assert that the query for any `expectedDeletedDocs` yields no results.
    if (expectedDeletedDocs.length > 0) {
        assert.soonNoExcept(() => {
            const deletedDocs = changeColl.aggregate(pipeline(expectedDeletedDocs)).toArray();
            assert.eq(deletedDocs.length, 0);
            return true;
        });
    }
}

// Returns the operation time for the provided document 'doc'.
function getDocumentOperationTime(doc) {
    const oplogEntry = primary.getDB("local").oplog.rs.findOne({o: doc});
    assert(oplogEntry);
    return oplogEntry.wall.getTime();
}

// Hard code a tenants information such that tenants can be identified deterministically.
const stocksTenantInfo = {
    tenantId: ObjectId("6303b6bb84305d2266d0b779"),
    user: "stock"
};
const citiesTenantInfo = {
    tenantId: ObjectId("7303b6bb84305d2266d0b779"),
    user: "cities"
};
const notUsedTenantInfo = {
    tenantId: ObjectId("8303b6bb84305d2266d0b779"),
    user: "notUser"
};

// Create connections to the primary such that they have respective tenant ids stamped.
const stocksTenantConnPrimary =
    getTenantConnection(primary.host, stocksTenantInfo.tenantId, stocksTenantInfo.user);
const citiesTenantConnPrimary =
    getTenantConnection(primary.host, citiesTenantInfo.tenantId, citiesTenantInfo.user);

// Create a tenant connection associated with 'notUsedTenantId' such that only the tenant id exists
// in the replica set but no corresponding change collection exists. The purging job should safely
// ignore this tenant without any side-effects.
const notUsedTenantConnPrimary =
    getTenantConnection(primary.host, notUsedTenantInfo.tenantId, notUsedTenantInfo.user);

// Create connections to the secondary such that they have respective tenant ids stamped.
const stocksTenantConnSecondary =
    getTenantConnection(secondary.host, stocksTenantInfo.tenantId, stocksTenantInfo.user);
const citiesTenantConnSecondary =
    getTenantConnection(secondary.host, citiesTenantInfo.tenantId, citiesTenantInfo.user);

// Enable change streams for both tenants.
replSet.setChangeStreamState(stocksTenantConnPrimary, true);
replSet.setChangeStreamState(citiesTenantConnPrimary, true);

// Verify change streams state for all tenants.
assert.eq(replSet.getChangeStreamState(stocksTenantConnPrimary), true);
assert.eq(replSet.getChangeStreamState(citiesTenantConnPrimary), true);
assert.eq(replSet.getChangeStreamState(notUsedTenantConnPrimary), false);

// Get tenants respective change collections on the primary.
const stocksChangeCollectionPrimary =
    stocksTenantConnPrimary.getDB("config").system.change_collection;
const citiesChangeCollectionPrimary =
    citiesTenantConnPrimary.getDB("config").system.change_collection;

// Get tenants respective change collections on the secondary.
const stocksChangeCollectionSecondary =
    stocksTenantConnSecondary.getDB("config").system.change_collection;
const citiesChangeCollectionSecondary =
    citiesTenantConnSecondary.getDB("config").system.change_collection;

// Set the 'expireAfterSeconds' to 'kExpireAfterSeconds'.
assert.commandWorked(stocksTenantConnPrimary.getDB("admin").runCommand(
    {setClusterParameter: {changeStreams: {expireAfterSeconds: kExpireAfterSeconds}}}));
assert.commandWorked(citiesTenantConnPrimary.getDB("admin").runCommand(
    {setClusterParameter: {changeStreams: {expireAfterSeconds: kExpireAfterSeconds}}}));

// Get tenants respective collections for testing.
const stocksTestDb = stocksTenantConnPrimary.getDB(jsTestName());
const citiesTestDb = citiesTenantConnPrimary.getDB(jsTestName());
const notUsedTestDb = notUsedTenantConnPrimary.getDB(jsTestName());

const stocksColl = assertDropAndRecreateCollection(stocksTestDb, "stocks");
const citiesColl = assertDropAndRecreateCollection(citiesTestDb, "cities");
const notUsedColl = assertDropAndRecreateCollection(notUsedTestDb, "notUsed");

// Wait until the remover job hangs.
let fpHangBeforeRemovingDocsPrimary =
    configureFailPoint(primary, "hangBeforeRemovingExpiredChanges");
let fpHangBeforeRemovingDocsSecondary =
    configureFailPoint(secondary, "hangBeforeRemovingExpiredChanges");
fpHangBeforeRemovingDocsPrimary.wait();
fpHangBeforeRemovingDocsSecondary.wait();

// Insert 5 documents to the 'stocks' collection owned by the 'stocksTenantId' that should be
// deleted.
const stocksExpiredDocuments = [
    {_id: "aapl", price: 140},
    {_id: "dis", price: 100},
    {_id: "nflx", price: 185},
    {_id: "baba", price: 66},
    {_id: "amc", price: 185}
];

// Insert 4 documents to the 'cities' collection owned by the 'citiesTenantId' that should be
// deleted.
const citiesExpiredDocuments = [
    {_id: "toronto", area_km2: 630},
    {_id: "singapore ", area_km2: 728},
    {_id: "london", area_km2: 1572},
    {_id: "tokyo", area_km2: 2194}
];

// Insert documents to the 'stocks' collection and wait for the replication.
assert.commandWorked(stocksColl.insertMany(stocksExpiredDocuments));
replSet.awaitReplication();

// Verify that the change collection for the 'stocks' tenant is consistent on both the primary and
// the secondary.
assertChangeCollectionDocuments(stocksChangeCollectionPrimary,
                                stocksColl,
                                /* expectedDeletedDocs */[],
                                /* expectedRetainedDocs */ stocksExpiredDocuments);
assertChangeCollectionDocuments(stocksChangeCollectionSecondary,
                                stocksColl,
                                /* expectedDeletedDocs */[],
                                /* expectedRetainedDocs */ stocksExpiredDocuments);

// Insert documents to the 'cities' collection and wait for the replication.
assert.commandWorked(citiesColl.insertMany(citiesExpiredDocuments));
replSet.awaitReplication();

// Verify that the change collection for the 'cities' tenant is consistent on both the primary and
// the secondary.
assertChangeCollectionDocuments(citiesChangeCollectionPrimary,
                                citiesColl,
                                /* expectedDeletedDocs */[],
                                /* expectedRetainedDocs */ citiesExpiredDocuments);
assertChangeCollectionDocuments(citiesChangeCollectionSecondary,
                                citiesColl,
                                /* expectedDeletedDocs */[],
                                /* expectedRetainedDocs */ citiesExpiredDocuments);

// Insert 2 documents to the 'notUsed' collection such that the associated tenant becomes visible to
// the mongoD. The documents in these collections will not be consumed by the change stream.
const notUsedDocuments =
    [{_id: "cricket_bat", since_years: 2}, {_id: "tennis_racket", since_years: 2}];
assert.commandWorked(notUsedColl.insertMany(notUsedDocuments));

// All document before and inclusive this wall time will be deleted by the purging job.
const lastExpiredDocumentTime = getDocumentOperationTime(citiesExpiredDocuments.at(-1));

// Sleep for 'kSleepBetweenWritesSeconds' duration such that the next batch of inserts
// has a sufficient delay in their wall time relative to the previous batch.
sleep(kSleepBetweenWritesSeconds * 1000);

// The documents for the 'stocks' collection owned by the 'stocksTenantId' that should not be
// deleted.
const stocksNonExpiredDocuments = [
    {_id: "wmt", price: 11},
    {_id: "coin", price: 23},
    {_id: "ddog", price: 15},
    {_id: "goog", price: 199},
    {_id: "tsla", price: 12}
];

// The documents for the 'cities' collection owned by the 'citiesTenantId' that should not be
// deleted.
const citiesNonExpiredDocuments = [
    {_id: "dublin", area_km2: 117},
    {_id: "new york", area_km2: 783},
    {_id: "hong kong", area_km2: 1114},
    {_id: "sydney", area_km2: 12386}
];

// Insert documents to the 'stocks' collection and wait for the replication.
assert.commandWorked(stocksColl.insertMany(stocksNonExpiredDocuments));
replSet.awaitReplication();

// Verify that state of change collection both at the primary and the secondary.
assertChangeCollectionDocuments(stocksChangeCollectionPrimary,
                                stocksColl,
                                /* expectedDeletedDocs */[],
                                /* expectedRetainedDocs */ stocksNonExpiredDocuments);
assertChangeCollectionDocuments(stocksChangeCollectionSecondary,
                                stocksColl,
                                /* expectedDeletedDocs */[],
                                /* expectedRetainedDocs */ stocksNonExpiredDocuments);

// Insert documents to the 'cities' collection and wait for the replication.
assert.commandWorked(citiesColl.insertMany(citiesNonExpiredDocuments));
replSet.awaitReplication();

// Verify that state of change collection both at the primary and the secondary.
assertChangeCollectionDocuments(citiesChangeCollectionPrimary,
                                citiesColl,
                                /* expectedDeletedDocs */[],
                                /* expectedRetainedDocs */ citiesNonExpiredDocuments);
assertChangeCollectionDocuments(citiesChangeCollectionSecondary,
                                citiesColl,
                                /* expectedDeletedDocs */[],
                                /* expectedRetainedDocs */ citiesNonExpiredDocuments);

// Calculate the 'currentWallTime' such that only the first batch of inserted documents
// should be expired, ie.: 'lastExpiredDocumentTime' + 'kExpireAfterSeconds' <
// 'currentWallTime' < first-non-expired-document.
const currentWallTime =
    new Date(lastExpiredDocumentTime + kExpireAfterSeconds * 1000 + kSafetyMarginMillis);
const failpointName =
    FeatureFlagUtil.isPresentAndEnabled(stocksTestDb, "UseUnreplicatedTruncatesForDeletions")
    ? "injectCurrentWallTimeForCheckingMarkers"
    : "injectCurrentWallTimeForRemovingExpiredDocuments";
const fpInjectWallTimePrimary = configureFailPoint(primary, failpointName, {currentWallTime});
const fpInjectWallTimeSecondary = configureFailPoint(secondary, failpointName, {currentWallTime});

// Unblock the change collection remover job such that it picks up on the injected
// 'currentWallTime'.
fpHangBeforeRemovingDocsPrimary.off();
fpHangBeforeRemovingDocsSecondary.off();

// Wait until the remover job has retrieved the injected 'currentWallTime' and reset the first
// failpoint.
fpInjectWallTimePrimary.wait();
fpInjectWallTimeSecondary.wait();

// Wait for a complete cycle of the TTL job.
fpHangBeforeRemovingDocsPrimary = configureFailPoint(primary, "hangBeforeRemovingExpiredChanges");
fpHangBeforeRemovingDocsSecondary =
    configureFailPoint(secondary, "hangBeforeRemovingExpiredChanges");
fpHangBeforeRemovingDocsPrimary.wait();
fpHangBeforeRemovingDocsSecondary.wait();

// Assert that only required documents are retained in change collections on the primary.
assertChangeCollectionDocuments(
    stocksChangeCollectionPrimary, stocksColl, stocksExpiredDocuments, stocksNonExpiredDocuments);
assertChangeCollectionDocuments(
    citiesChangeCollectionPrimary, citiesColl, citiesExpiredDocuments, citiesNonExpiredDocuments);

// Wait for the replication to complete and assert that the expired documents have also been deleted
// from the secondary and the state is consistent with the primary.
replSet.awaitReplication();
assertChangeCollectionDocuments(
    stocksChangeCollectionSecondary, stocksColl, stocksExpiredDocuments, stocksNonExpiredDocuments);
assertChangeCollectionDocuments(
    citiesChangeCollectionSecondary, citiesColl, citiesExpiredDocuments, citiesNonExpiredDocuments);

fpHangBeforeRemovingDocsPrimary.off();
fpHangBeforeRemovingDocsSecondary.off();

replSet.stopSet();
})();
