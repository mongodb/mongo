/**
 * Tests that a time-series collection created with the 'expireAfterSeconds' option will remove
 * buckets older than 'expireAfterSeconds' based on the bucket creation time while also regarding
 * the partial filter on the metafield.
 *
 * @tags: [
 *   does_not_support_stepdowns,
 *   does_not_support_transactions,
 *   featureFlagTimeseriesScalabilityImprovements
 * ]
 */
(function() {
"use strict";

load('jstests/libs/fixture_helpers.js');  // For 'FixtureHelpers'
load("jstests/libs/clustered_collections/clustered_collection_util.js");
load("jstests/core/timeseries/libs/timeseries.js");
load("jstests/libs/ttl_util.js");

const conn = MongoRunner.runMongod({setParameter: 'ttlMonitorSleepSecs=1'});
const testDB = conn.getDB(jsTestName());
assert.commandWorked(testDB.dropDatabase());

TimeseriesTest.run((insert) => {
    const coll = testDB.timeseries_expires_with_partial_index;
    const bucketsColl = testDB.getCollection('system.buckets.' + coll.getName());

    const timeFieldName = 'tm';
    const metaFieldName = "mm";
    const indexName = "partialTTLIndex";
    const timeSpec = {[timeFieldName]: 1};
    const expireAfterSecond = NumberLong(1);
    const expireAfterSeconds = NumberLong(10000);

    const startDate = new Date();
    const expiredDate = new Date(startDate - ((expireAfterSeconds / 2) * 1000));
    const collectionTTLExpiredDate = new Date(startDate - ((expireAfterSeconds * 2) * 1000));
    const futureDate = new Date(startDate.getTime() + (10000 * 10));

    assert.lt(expiredDate, startDate);
    assert.gt(futureDate, startDate);

    const expiredDoc = {_id: 0, [timeFieldName]: expiredDate, [metaFieldName]: 8, x: 0};
    const expiredDocLowMeta = {_id: 1, [timeFieldName]: expiredDate, [metaFieldName]: 0, x: 1};
    const collectionTTLExpiredDocLowMeta =
        {_id: 2, [timeFieldName]: collectionTTLExpiredDate, [metaFieldName]: 0, x: 2};
    const futureDoc = {_id: 3, [timeFieldName]: futureDate, [metaFieldName]: 10, x: 3};

    const partialIndexOptions = {
        name: indexName,
        partialFilterExpression: {[metaFieldName]: {$gt: 5}},
        expireAfterSeconds: expireAfterSecond
    };

    const checkInsertion = function(coll, doc, expectDeletion) {
        jsTestLog("Inserting doc into collection.");
        const prevCount = bucketsColl.find().itcount();
        assert.commandWorked(insert(coll, doc), 'failed to insert doc: ' + tojson(doc));

        // Wait for the TTL monitor to process the indexes.
        jsTestLog("Waiting for TTL monitor to process...");
        TTLUtil.waitForPass(testDB);

        // Check the number of bucket documents.
        const expectedCount = (expectDeletion) ? prevCount : prevCount + 1;
        const bucketDocs = bucketsColl.find().sort({'control.min._id': 1}).toArray();

        assert.eq(expectedCount, bucketDocs.length, bucketDocs);
        jsTestLog("Doc deleted: " + expectDeletion + ".");
    };

    const testTTLIndex = function(coll) {
        // Inserts a measurement with a time in the past to ensure the measurement will be removed
        // immediately.
        checkInsertion(coll, expiredDoc, true);

        // Inserts a measurement that does not meet the partialFilterExpression to ensure it will
        // not be removed (even though it is 'expired').
        checkInsertion(coll, expiredDocLowMeta, false);

        // Inserts a measurement with a time in the future to ensure the measurement is not removed.
        checkInsertion(coll, futureDoc, false);
    };

    {
        coll.drop();
        assert.commandWorked(testDB.createCollection(
            coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));
        assert.contains(bucketsColl.getName(), testDB.getCollectionNames());

        // Create a TTL index on time, with a partial filter expression on the metaField.
        assert.commandWorked(coll.createIndex(timeSpec, partialIndexOptions));
    }

    // Test the TTL Deleter on a time-series collection with a TTL index and partialFilter.
    testTTLIndex(coll);

    {
        coll.drop();
        assert.commandWorked(testDB.createCollection(coll.getName(), {
            timeseries: {timeField: timeFieldName, metaField: metaFieldName},
            expireAfterSeconds: expireAfterSeconds
        }));
        assert.contains(bucketsColl.getName(), testDB.getCollectionNames());

        // Create a secondary TTL index on time, with a partial filter expression on the metaField.
        assert.commandWorked(coll.createIndex(timeSpec, partialIndexOptions));
    }

    // Test the TTL Deleter on a time-series collection with a TTL index and partialFilter and a
    // pre-existing TTL index.
    testTTLIndex(coll);

    // As a sanity check, check that the TTL deleter deletes a document that does not match partial
    // filter but is expired, with respect to the collection TTL index.
    checkInsertion(coll, collectionTTLExpiredDocLowMeta, true);
});

MongoRunner.stopMongod(conn);
})();
