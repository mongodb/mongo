/**
 * Tests the change collection periodic remover job.
 *
 * @tags: [requires_fcv_61]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");           // For configureFailPoint.
load("jstests/libs/collection_drop_recreate.js");  // For assertDropAndRecreateCollection.

const kExpiredChangeRemovalJobSleepSeconds = 5;
const kExpireAfterSeconds = 1;
const kSleepBetweenWritesSeconds = 5;
const kSafetyMarginMillis = 1;

const rst = new ReplSetTest({nodes: 2});

// TODO SERVER-67267: Add 'featureFlagServerlessChangeStreams', 'multitenancySupport' and
// 'serverless' flags and remove 'failpoint.forceEnableChangeCollectionsMode'.
rst.startSet({
    setParameter: {
        "failpoint.forceEnableChangeCollectionsMode": tojson({mode: "alwaysOn"}),
        changeCollectionRemoverJobSleepSeconds: kExpiredChangeRemovalJobSleepSeconds
    }
});
rst.initiate();

const primary = rst.getPrimary();
const secondary = rst.getSecondary();
const testDb = primary.getDB(jsTestName());

// Enable change streams to ensure the creation of change collections.
assert.commandWorked(primary.getDB("admin").runCommand({setChangeStreamState: 1, enabled: true}));

// Set the 'expireAfterSeconds' to 'kExpireAfterSeconds'.
assert.commandWorked(primary.getDB("admin").runCommand(
    {setClusterParameter: {changeStreams: {expireAfterSeconds: kExpireAfterSeconds}}}));

// TODO SERVER-65950 Extend the test case to account for multi-tenancy.
const primaryChangeCollection = primary.getDB("config").system.change_collection;
const secondaryChangeCollection = secondary.getDB("config").system.change_collection;

// Assert that the change collection contains all documents in 'expectedRetainedDocs' and no
// document in 'expectedDeletedDocs' for the collection 'testColl'.
function assertChangeCollectionDocuments(
    changeColl, testColl, expectedDeletedDocs, expectedRetainedDocs) {
    const collNss = `${testDb.getName()}.${testColl.getName()}`;
    const pipeline = (collectionEntries) => [{$match: {op: "i", ns: collNss}},
                                             {$replaceRoot: {"newRoot": "$o"}},
                                             {$match: {$or: collectionEntries}}];

    // Assert that querying for 'expectedRetainedDocs' yields documents that are exactly the same as
    // 'expectedRetainedDocs'.
    if (expectedRetainedDocs.length > 0) {
        const retainedDocs = changeColl.aggregate(pipeline(expectedRetainedDocs)).toArray();
        assert.eq(retainedDocs, expectedRetainedDocs);
    }

    // Assert that the query for any `expectedDeletedDocs` yields no results.
    if (expectedDeletedDocs.length > 0) {
        const deletedDocs = changeColl.aggregate(pipeline(expectedDeletedDocs)).toArray();
        assert.eq(deletedDocs.length, 0);
    }
}

// Returns the operation time for the provided document 'doc'.
function getDocumentOperationTime(doc) {
    const oplogEntry = primary.getDB("local").oplog.rs.findOne({o: doc});
    assert(oplogEntry);
    return oplogEntry.wall.getTime();
}

(function testOnlyExpiredDocumentsDeleted() {
    assertDropAndRecreateCollection(testDb, "stocks");
    const testColl = testDb.stocks;

    // Wait until the remover job hangs.
    let fpHangBeforeRemovingDocs = configureFailPoint(primary, "hangBeforeRemovingExpiredChanges");
    fpHangBeforeRemovingDocs.wait();

    // Insert 5 documents.
    const expiredDocuments = [
        {_id: "aapl", price: 140},
        {_id: "dis", price: 100},
        {_id: "nflx", price: 185},
        {_id: "baba", price: 66},
        {_id: "amc", price: 185}
    ];

    assert.commandWorked(testColl.insertMany(expiredDocuments));
    assertChangeCollectionDocuments(primaryChangeCollection,
                                    testColl,
                                    /* expectedDeletedDocs */[],
                                    /* expectedRetainedDocs */ expiredDocuments);
    const lastExpiredDocumentTime = getDocumentOperationTime(expiredDocuments.at(-1));

    // Sleep for 'kSleepBetweenWritesSeconds' duration such that the next batch of inserts
    // has a sufficient delay in their wall time relative to the previous batch.
    sleep(kSleepBetweenWritesSeconds * 1000);

    // Insert 5 more documents.
    const nonExpiredDocuments = [
        {_id: "wmt", price: 11},
        {_id: "coin", price: 23},
        {_id: "ddog", price: 15},
        {_id: "goog", price: 199},
        {_id: "tsla", price: 12}
    ];

    assert.commandWorked(testColl.insertMany(nonExpiredDocuments));
    assertChangeCollectionDocuments(primaryChangeCollection,
                                    testColl,
                                    /* expectedDeletedDocs */[],
                                    /* expectedRetainedDocs */ nonExpiredDocuments);

    // Calculate the 'currentWallTime' such that only the first batch of inserted documents
    // should be expired, ie.: 'lastExpiredDocumentTime' + 'kExpireAfterSeconds' <
    // 'currentWallTime' < 'firstNonExpiredDocument'
    const currentWallTime =
        new Date(lastExpiredDocumentTime + kExpireAfterSeconds * 1000 + kSafetyMarginMillis);
    const fpInjectWallTime = configureFailPoint(
        primary, "injectCurrentWallTimeForRemovingExpiredDocuments", {currentWallTime});

    // Unblock the change collection remover job such that it picks up on the injected
    // 'currentWallTime'.
    fpHangBeforeRemovingDocs.off();

    // Wait until the remover job has retrieved the injected 'currentWallTime' and reset the first
    // failpoint.
    fpInjectWallTime.wait();

    // Wait for a complete cycle of the TTL job.
    fpHangBeforeRemovingDocs = configureFailPoint(primary, "hangBeforeRemovingExpiredChanges");
    fpHangBeforeRemovingDocs.wait();

    // Assert that the first 5 documents got deleted, but the later 5 documents did not.
    assertChangeCollectionDocuments(
        primaryChangeCollection, testColl, expiredDocuments, nonExpiredDocuments);

    // Wait for the replication to complete and assert that the expired documents also have been
    // deleted from the secondary.
    rst.awaitReplication();
    assertChangeCollectionDocuments(
        secondaryChangeCollection, testColl, expiredDocuments, nonExpiredDocuments);
    fpHangBeforeRemovingDocs.off();
})();

rst.stopSet();
})();
