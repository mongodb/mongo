/**
 * Tests the behavior of TTL _id on time-series collections. Ensures that data is only expired when
 * it is guaranteed to be past the maximum time range of a bucket.
 *
 * @tags: [
 *   assumes_no_implicit_collection_creation_after_drop,
 *   does_not_support_stepdowns,
 *   requires_getmore,
 * ]
 */
(function() {
"use strict";
load("jstests/libs/clustered_collections/clustered_collection_util.js");

// Run TTL monitor constantly to speed up this test.
const conn = MongoRunner.runMongod({setParameter: 'ttlMonitorSleepSecs=1'});

const dbName = jsTestName();
const testDB = conn.getDB(dbName);

const timeFieldName = 'time';
const metaFieldName = 'host';
const expireAfterSeconds = 5;
// Default maximum range of time for a bucket.
const defaultBucketMaxRange = 3600;

const testCase = (testFn) => {
    const coll = testDB.getCollection('ts');
    const bucketsColl = testDB.getCollection('system.buckets.' + coll.getName());
    assert.commandWorked(testDB.createCollection(coll.getName(), {
        timeseries: {
            timeField: timeFieldName,
            metaField: metaFieldName,
        },
        expireAfterSeconds: expireAfterSeconds,
    }));

    testFn(coll, bucketsColl);
    assert(coll.drop());
};

testCase((coll, bucketsColl) => {
    // Insert two measurements that end up in the same bucket, but where the minimum is 5 minutes
    // earlier. Expect that the TTL monitor does not delete the data even though the bucket minimum
    // is past the expiry.
    const maxTime = new Date();
    const minTime = new Date(maxTime.getTime() - (1000 * 5 * 60));
    assert.commandWorked(coll.insert({[timeFieldName]: minTime, [metaFieldName]: "localhost"}));
    assert.commandWorked(coll.insert({[timeFieldName]: maxTime, [metaFieldName]: "localhost"}));
    assert.eq(2, coll.find().itcount());
    assert.eq(1, bucketsColl.find().itcount());

    ClusteredCollectionUtil.waitForTTL(coll.getDB("test"));
    assert.eq(2, coll.find().itcount());
    assert.eq(1, bucketsColl.find().itcount());
});

testCase((coll, bucketsColl) => {
    // Insert two measurements 5 minutes apart that end up in the same bucket and both are older
    // than the TTL expiry. Expect that the TTL monitor does not delete the data even though the
    // bucket minimum is past the expiry because it is waiting for the maximum bucket range to
    // elapse.
    const maxTime = new Date((new Date()).getTime() - (1000 * expireAfterSeconds));
    const minTime = new Date(maxTime.getTime() - (1000 * 5 * 60));
    assert.commandWorked(coll.insert({[timeFieldName]: minTime, [metaFieldName]: "localhost"}));
    assert.commandWorked(coll.insert({[timeFieldName]: maxTime, [metaFieldName]: "localhost"}));
    assert.eq(2, coll.find().itcount());
    assert.eq(1, bucketsColl.find().itcount());

    ClusteredCollectionUtil.waitForTTL(coll.getDB("test"));
    assert.eq(2, coll.find().itcount());
    assert.eq(1, bucketsColl.find().itcount());
});

testCase((coll, bucketsColl) => {
    // Insert two measurements 5 minutes apart that end up in the same bucket and both are older
    // than the TTL expiry and the maximum bucket range. Expect that the TTL monitor deletes the
    // data because the bucket minimum is past the expiry plus the maximum bucket range.
    const maxTime = new Date((new Date()).getTime() - (1000 * defaultBucketMaxRange));
    const minTime = new Date(maxTime.getTime() - (1000 * 5 * 60));
    assert.commandWorked(coll.insert({[timeFieldName]: minTime, [metaFieldName]: "localhost"}));
    assert.commandWorked(coll.insert({[timeFieldName]: maxTime, [metaFieldName]: "localhost"}));

    ClusteredCollectionUtil.waitForTTL(coll.getDB("test"));
    assert.eq(0, coll.find().itcount());
    assert.eq(0, bucketsColl.find().itcount());
});

testCase((coll, bucketsColl) => {
    // Insert two measurements using insertMany that end up in the same bucket, but where the
    // minimum is 5 minutes earlier. Expect that the TTL monitor does not delete the data even
    // though the bucket minimum is past the expiry.
    const maxTime = new Date();
    const minTime = new Date(maxTime.getTime() - (1000 * 5 * 60));
    assert.commandWorked(coll.insertMany([
        {[timeFieldName]: minTime, [metaFieldName]: "localhost"},
        {[timeFieldName]: maxTime, [metaFieldName]: "localhost"}
    ]));

    assert.eq(2, coll.find().itcount());
    assert.eq(1, bucketsColl.find().itcount());

    ClusteredCollectionUtil.waitForTTL(coll.getDB("test"));
    assert.eq(2, coll.find().itcount());
    assert.eq(1, bucketsColl.find().itcount());
});

testCase((coll, bucketsColl) => {
    // Insert two measurements with insertMany 5 minutes apart that end up in the same bucket and
    // both are older than the TTL expiry and the maximum bucket range. Expect that the TTL monitor
    // deletes the data because the bucket minimum is past the expiry plus the maximum bucket range.
    const maxTime = new Date((new Date()).getTime() - (1000 * defaultBucketMaxRange));
    const minTime = new Date(maxTime.getTime() - (1000 * 5 * 60));
    assert.commandWorked(coll.insertMany([
        {[timeFieldName]: minTime, [metaFieldName]: "localhost"},
        {[timeFieldName]: maxTime, [metaFieldName]: "localhost"}
    ]));

    ClusteredCollectionUtil.waitForTTL(coll.getDB("test"));
    assert.eq(0, coll.find().itcount());
    assert.eq(0, bucketsColl.find().itcount());
});

// Make a collection TTL using collMod. Ensure data expires correctly.
(function newlyTTLWithCollMod() {
    const coll = testDB.getCollection('ts');
    const bucketsColl = testDB.getCollection('system.buckets.' + coll.getName());
    assert.commandWorked(testDB.createCollection(coll.getName(), {
        timeseries: {
            timeField: timeFieldName,
            metaField: metaFieldName,
        }
    }));

    // Insert two measurements 5 minutes apart that end up in the same bucket and both are older
    // than the TTL expiry and the maximum bucket range.
    const maxTime = new Date((new Date()).getTime() - (1000 * defaultBucketMaxRange));
    const minTime = new Date(maxTime.getTime() - (1000 * 5 * 60));
    assert.commandWorked(coll.insert({[timeFieldName]: minTime, [metaFieldName]: "localhost"}));
    assert.commandWorked(coll.insert({[timeFieldName]: maxTime, [metaFieldName]: "localhost"}));

    assert.eq(2, coll.find().itcount());
    assert.eq(1, bucketsColl.find().itcount());

    // Make the collection TTL and expect the data to be deleted because the bucket minimum is past
    // the expiry plus the maximum bucket range.
    assert.commandWorked(testDB.runCommand({
        collMod: 'ts',
        expireAfterSeconds: expireAfterSeconds,
    }));

    ClusteredCollectionUtil.waitForTTL(coll.getDB("test"));
    assert.eq(0, coll.find().itcount());
    assert.eq(0, bucketsColl.find().itcount());
})();

MongoRunner.stopMongod(conn);
})();
