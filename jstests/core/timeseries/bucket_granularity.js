/**
 * Tests correctness of time-series bucket granularity configuration.
 *
 * @tags: [
 *   # This test depends on certain writes ending up in the same bucket. Stepdowns and tenant
 *   # migrations may result in writes splitting between two primaries, and thus different buckets.
 *   does_not_support_stepdowns,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   # This test calls "find" with a filter on "_id" whose value is a namespace string. We cannot
 *   # test it as the override does not inject tenant prefix to this special namespace.
 *   simulate_atlas_proxy_incompatible,
 * ]
 */

function verifyViewPipeline(coll) {
    const cProps =
        db.runCommand({listCollections: 1, filter: {name: coll.getName()}}).cursor.firstBatch[0];
    const cSeconds = cProps.options.timeseries.bucketMaxSpanSeconds;
    const vProps = db.system.views.find({_id: `${db.getName()}.${coll.getName()}`}).toArray()[0];
    const vSeconds = vProps.pipeline[0].$_internalUnpackBucket.bucketMaxSpanSeconds;
    assert.eq(cSeconds,
              vSeconds,
              `expected view pipeline 'bucketMaxSpanSeconds' to match timeseries options`);
}

function getDateOutsideBucketRange(coll, spanMS) {
    const newestBucketDoc = coll.find().sort({"control.min.t": -1}).limit(1).toArray()[0];
    let newDate = new Date(Date.parse(newestBucketDoc.control.min.t) + spanMS);
    return ISODate(newDate.toISOString());
}

const dayInMS = 1000 * 60 * 60 * 24;

(function testSeconds() {
    let coll = db[jsTestName() + "_granularitySeconds"];
    let bucketsColl = db.getCollection('system.buckets.' + coll.getName());
    coll.drop();

    assert.commandWorked(db.createCollection(
        coll.getName(), {timeseries: {timeField: 't', granularity: 'seconds'}}));
    if (TestData.runningWithBalancer) {
        // In suites running moveCollection in the background, it is possible to hit the issue
        // described by SERVER-89349 which will result in more bucket documents being created.
        // Creating an index on the time field allows the buckets to be reopened, allowing the
        // counts in this test to be accurate.
        assert.commandWorked(coll.createIndex({'t': 1}));
    }

    // All measurements land in the same bucket.
    assert.commandWorked(coll.insert({t: ISODate("2021-04-22T20:00:00.000Z")}));
    assert.commandWorked(coll.insert({t: ISODate("2021-04-22T20:00:03.000Z")}));
    assert.commandWorked(coll.insert({t: ISODate("2021-04-22T20:00:59.999Z")}));
    assert.eq(1, bucketsColl.find().itcount());

    // Expect bucket max span to be one hour. A new measurement outside of this range should create
    // a new bucket.
    assert.commandWorked(coll.insert({t: ISODate("2021-04-22T20:59:59.999Z")}));
    assert.eq(1, bucketsColl.find().itcount());
    assert.commandWorked(coll.insert({t: ISODate("2021-04-22T21:00:00.000Z")}));
    assert.eq(2, bucketsColl.find().itcount());
})();

(function testMinutes() {
    let coll = db[jsTestName() + "_granularityMinutes"];
    let bucketsColl = db.getCollection('system.buckets.' + coll.getName());
    coll.drop();

    assert.commandWorked(db.createCollection(
        coll.getName(), {timeseries: {timeField: 't', granularity: 'minutes'}}));
    if (TestData.runningWithBalancer) {
        assert.commandWorked(coll.createIndex({'t': 1}));
    }

    // All measurements land in the same bucket.
    assert.commandWorked(coll.insert({t: ISODate("2021-04-22T20:00:00.000Z")}));
    assert.commandWorked(coll.insert({t: ISODate("2021-04-22T20:22:02.000Z")}));
    assert.commandWorked(coll.insert({t: ISODate("2021-04-22T20:59:59.999Z")}));
    assert.eq(1, bucketsColl.find().itcount());

    // Expect bucket max span to be one day. A new measurement outside of this range should create
    // a new bucket.
    assert.commandWorked(coll.insert({t: ISODate("2021-04-23T19:59:59.999Z")}));
    assert.eq(1, bucketsColl.find().itcount());
    assert.commandWorked(coll.insert({t: ISODate("2021-04-23T20:00:00.000Z")}));
    assert.eq(2, bucketsColl.find().itcount());
})();

(function testHours() {
    let coll = db[jsTestName() + "_granularityHours"];
    let bucketsColl = db.getCollection('system.buckets.' + coll.getName());
    coll.drop();

    assert.commandWorked(
        db.createCollection(coll.getName(), {timeseries: {timeField: 't', granularity: 'hours'}}));
    if (TestData.runningWithBalancer) {
        assert.commandWorked(coll.createIndex({'t': 1}));
    }

    // All measurements land in the same bucket.
    assert.commandWorked(coll.insert({t: ISODate("2021-04-22T00:00:00.000Z")}));
    assert.commandWorked(coll.insert({t: ISODate("2021-04-22T20:11:03.000Z")}));
    assert.commandWorked(coll.insert({t: ISODate("2021-04-22T23:59:59.999Z")}));
    assert.eq(1, bucketsColl.find().itcount());

    // Expect bucket max span to be 30 days. A new measurement outside of this range should create
    // a new bucket.
    assert.commandWorked(coll.insert({t: ISODate("2021-05-21T23:59:59.999Z")}));
    assert.eq(1, bucketsColl.find().itcount());
    assert.commandWorked(coll.insert({t: ISODate("2021-05-22T00:00:00.000Z")}));
    assert.eq(2, bucketsColl.find().itcount());
})();

(function testIncreasingSecondsToMinutes() {
    let coll = db[jsTestName() + "_granularitySecondsToMinutes"];
    let bucketsColl = db.getCollection('system.buckets.' + coll.getName());
    coll.drop();

    assert.commandWorked(db.createCollection(
        coll.getName(), {timeseries: {timeField: 't', granularity: 'seconds'}}));
    if (TestData.runningWithBalancer) {
        assert.commandWorked(coll.createIndex({'t': 1}));
    }
    verifyViewPipeline(coll);

    // All measurements land in the same bucket.
    assert.commandWorked(coll.insert({t: ISODate("2021-04-22T20:00:00.000Z")}));
    assert.commandWorked(coll.insert({t: ISODate("2021-04-22T20:00:03.000Z")}));
    assert.commandWorked(coll.insert({t: ISODate("2021-04-22T20:00:59.999Z")}));
    assert.eq(1, bucketsColl.find().itcount());

    // Expect bucket max span to be one hour. A new measurement outside of this range should create
    // a new bucket.
    assert.commandWorked(coll.insert({t: ISODate("2021-04-22T20:59:59.999Z")}));
    assert.eq(1, bucketsColl.find().itcount());
    assert.commandWorked(coll.insert({t: ISODate("2021-04-22T21:00:00.000Z")}));
    assert.eq(2, bucketsColl.find().itcount());

    // Now let's bump to minutes and make sure we get the expected behavior
    assert.commandWorked(
        db.runCommand({collMod: coll.getName(), timeseries: {granularity: 'minutes'}}));
    verifyViewPipeline(coll);

    // If resharding is running in the background, it could be that neither of the buckets are in
    // memory here. If this is the case, the first insert will load the first bucket into memory but
    // not the second. This can cause the third insert to attempt to insert into the first bucket
    // rather than the second, creating 3 buckets rather than keeping 2.
    assert.commandWorked(coll.insert({t: ISODate("2021-04-23T20:00:00.000Z")}));
    assert.commandWorked(coll.insert({t: ISODate("2021-04-23T20:22:02.000Z")}));
    assert.commandWorked(coll.insert({t: ISODate("2021-04-23T20:59:59.999Z")}));
    let bucketCount = bucketsColl.find().itcount();
    if (TestData.runningWithBalancer) {
        assert.lte(2, bucketCount);
    } else {
        assert.eq(2, bucketCount);
    }

    // Expect bucket max span to be one day. A new measurement outside of this range should create
    // a new bucket. Don't hardcode this because if the balancer is on, we may have more than 2
    // buckets.
    let newDate = getDateOutsideBucketRange(bucketsColl, dayInMS);
    assert.commandWorked(coll.insert({t: newDate}));
    assert.eq(bucketCount + 1, bucketsColl.find().itcount());

    // Make sure when we query, we use the new bucket max span to make sure we get all matches
    assert.eq(4, coll.find({t: {$gt: ISODate("2021-04-23T19:00:00.000Z")}}).itcount());
})();

(function testIncreasingSecondsToHours() {
    let coll = db[jsTestName() + "_granularitySecondsToHours"];
    let bucketsColl = db.getCollection('system.buckets.' + coll.getName());
    coll.drop();

    assert.commandWorked(db.createCollection(
        coll.getName(), {timeseries: {timeField: 't', granularity: 'seconds'}}));
    if (TestData.runningWithBalancer) {
        assert.commandWorked(coll.createIndex({'t': 1}));
    }
    verifyViewPipeline(coll);

    // All measurements land in the same bucket.
    assert.commandWorked(coll.insert({t: ISODate("2021-04-22T20:00:00.000Z")}));
    assert.commandWorked(coll.insert({t: ISODate("2021-04-22T20:00:03.000Z")}));
    assert.commandWorked(coll.insert({t: ISODate("2021-04-22T20:00:59.999Z")}));
    assert.eq(1, bucketsColl.find().itcount());

    // Expect bucket max span to be one hour. A new measurement outside of this range should create
    // a new bucket.
    assert.commandWorked(coll.insert({t: ISODate("2021-04-22T20:59:59.999Z")}));
    assert.eq(1, bucketsColl.find().itcount());
    assert.commandWorked(coll.insert({t: ISODate("2021-04-22T21:00:00.000Z")}));
    assert.eq(2, bucketsColl.find().itcount());

    assert.commandWorked(
        db.runCommand({collMod: coll.getName(), timeseries: {granularity: 'hours'}}));
    verifyViewPipeline(coll);

    // All measurements land in the same bucket.
    assert.commandWorked(coll.insert({t: ISODate("2021-05-22T00:00:00.000Z")}));
    assert.commandWorked(coll.insert({t: ISODate("2021-05-22T18:11:03.000Z")}));
    assert.commandWorked(coll.insert({t: ISODate("2021-05-22T20:59:59.999Z")}));
    let bucketCount = bucketsColl.find().itcount();
    if (TestData.runningWithBalancer) {
        assert.lte(2, bucketCount);
    } else {
        assert.eq(2, bucketCount);
    }

    // Expect bucket max span to be 30 days. A new measurement outside of this range should create
    // a new bucket.
    let newDate = getDateOutsideBucketRange(bucketsColl, dayInMS * 30);
    assert.commandWorked(coll.insert({t: newDate}));
    assert.eq(bucketCount + 1, bucketsColl.find().itcount());

    // Make sure when we query, we use the new bucket max span to make sure we get all matches
    assert.eq(4, coll.find({t: {$gt: ISODate("2021-05-21T00:00:00.000Z")}}).itcount());
})();

(function testIncreasingMinutesToHours() {
    let coll = db[jsTestName() + "_granularityMinutesToHours"];
    let bucketsColl = db.getCollection('system.buckets.' + coll.getName());
    coll.drop();

    assert.commandWorked(db.createCollection(
        coll.getName(), {timeseries: {timeField: 't', granularity: 'minutes'}}));
    if (TestData.runningWithBalancer) {
        assert.commandWorked(coll.createIndex({'t': 1}));
    }
    verifyViewPipeline(coll);

    // All measurements land in the same bucket.
    assert.commandWorked(coll.insert({t: ISODate("2021-04-22T20:00:00.000Z")}));
    assert.commandWorked(coll.insert({t: ISODate("2021-04-22T20:22:02.000Z")}));
    assert.commandWorked(coll.insert({t: ISODate("2021-04-22T20:59:59.999Z")}));
    assert.eq(1, bucketsColl.find().itcount());

    // Expect bucket max span to be one day. A new measurement outside of this range should create
    // a new bucket.
    assert.commandWorked(coll.insert({t: ISODate("2021-04-23T19:59:59.999Z")}));
    assert.eq(1, bucketsColl.find().itcount());
    assert.commandWorked(coll.insert({t: ISODate("2021-04-23T20:00:00.000Z")}));
    assert.eq(2, bucketsColl.find().itcount());

    assert.commandWorked(
        db.runCommand({collMod: coll.getName(), timeseries: {granularity: 'hours'}}));
    verifyViewPipeline(coll);

    // All measurements land in the same bucket.
    assert.commandWorked(coll.insert({t: ISODate("2021-05-23T00:00:00.000Z")}));
    assert.commandWorked(coll.insert({t: ISODate("2021-05-23T18:11:03.000Z")}));
    assert.commandWorked(coll.insert({t: ISODate("2021-05-23T19:59:59.999Z")}));
    let bucketCount = bucketsColl.find().itcount();
    if (TestData.runningWithBalancer) {
        assert.lte(2, bucketCount);
    } else {
        assert.eq(2, bucketCount);
    }

    // Expect bucket max span to be 30 days. A new measurement outside of this range should create
    // a new bucket.
    let newDate = getDateOutsideBucketRange(bucketsColl, dayInMS * 30);
    assert.commandWorked(coll.insert({t: newDate}));
    assert.eq(bucketCount + 1, bucketsColl.find().itcount());

    // Make sure when we query, we use the new bucket max span to make sure we get all matches
    assert.eq(4, coll.find({t: {$gt: ISODate("2021-05-22T00:00:00.000Z")}}).itcount());
})();

(function testReducingGranularityFails() {
    let coll = db[jsTestName() + "_reducingGranularityFails"];
    coll.drop();

    assert.commandWorked(db.createCollection(
        coll.getName(), {timeseries: {timeField: 't', granularity: 'minutes'}}));
    if (TestData.runningWithBalancer) {
        assert.commandWorked(coll.createIndex({'t': 1}));
    }

    // Decreasing minutes -> seconds shouldn't work.
    assert.commandFailed(
        db.runCommand({collMod: coll.getName(), timeseries: {granularity: 'seconds'}}));

    // Increasing minutes -> hours should work fine.
    assert.commandWorked(
        db.runCommand({collMod: coll.getName(), timeseries: {granularity: 'hours'}}));

    // Decreasing hours -> minutes shouldn't work.
    assert.commandFailed(
        db.runCommand({collMod: coll.getName(), timeseries: {granularity: 'minutes'}}));
    // Decreasing hours -> seconds shouldn't work either.
    assert.commandFailed(
        db.runCommand({collMod: coll.getName(), timeseries: {granularity: 'seconds'}}));
})();
