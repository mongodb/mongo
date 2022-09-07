/**
 * Tests correctness of time-series bucket granularity configuration.
 *
 * @tags: [
 *   # This test depends on certain writes ending up in the same bucket. Stepdowns may result in
 *   # writes splitting between two primaries, and thus different buckets.
 *   does_not_support_stepdowns,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 */

(function() {

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

(function testSeconds() {
    let coll = db.bucket_granularity_granularitySeconds;
    let bucketsColl = db.getCollection('system.buckets.' + coll.getName());
    coll.drop();

    assert.commandWorked(db.createCollection(
        coll.getName(), {timeseries: {timeField: 't', granularity: 'seconds'}}));

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
    let coll = db.bucket_granularity_granularityMinutes;
    let bucketsColl = db.getCollection('system.buckets.' + coll.getName());
    coll.drop();

    assert.commandWorked(db.createCollection(
        coll.getName(), {timeseries: {timeField: 't', granularity: 'minutes'}}));

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
    let coll = db.bucket_granularity_granularityHours;
    let bucketsColl = db.getCollection('system.buckets.' + coll.getName());
    coll.drop();

    assert.commandWorked(
        db.createCollection(coll.getName(), {timeseries: {timeField: 't', granularity: 'hours'}}));

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
    let coll = db.bucket_granularity_granularitySecondsToMinutes;
    let bucketsColl = db.getCollection('system.buckets.' + coll.getName());
    coll.drop();

    assert.commandWorked(db.createCollection(
        coll.getName(), {timeseries: {timeField: 't', granularity: 'seconds'}}));
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

    // All measurements land in the same bucket.
    assert.commandWorked(coll.insert({t: ISODate("2021-04-23T20:00:00.000Z")}));
    assert.commandWorked(coll.insert({t: ISODate("2021-04-23T20:22:02.000Z")}));
    assert.commandWorked(coll.insert({t: ISODate("2021-04-23T20:59:59.999Z")}));
    assert.eq(2, bucketsColl.find().itcount());

    // Expect bucket max span to be one day. A new measurement outside of this range should create
    // a new bucket.
    assert.commandWorked(coll.insert({t: ISODate("2021-04-23T21:00:00.000Z")}));
    assert.eq(3, bucketsColl.find().itcount());

    // Make sure when we query, we use the new bucket max span to make sure we get all matches
    assert.eq(4, coll.find({t: {$gt: ISODate("2021-04-23T19:00:00.000Z")}}).itcount());
})();

(function testIncreasingSecondsToHours() {
    let coll = db.bucket_granularity_granularitySecondsToHours;
    let bucketsColl = db.getCollection('system.buckets.' + coll.getName());
    coll.drop();

    assert.commandWorked(db.createCollection(
        coll.getName(), {timeseries: {timeField: 't', granularity: 'seconds'}}));
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
    assert.eq(2, bucketsColl.find().itcount());

    // Expect bucket max span to be 30 days. A new measurement outside of this range should create
    // a new bucket.
    assert.commandWorked(coll.insert({t: ISODate("2021-05-22T21:00:00.001Z")}));
    assert.eq(3, bucketsColl.find().itcount());

    // Make sure when we query, we use the new bucket max span to make sure we get all matches
    assert.eq(4, coll.find({t: {$gt: ISODate("2021-05-21T00:00:00.000Z")}}).itcount());
})();

(function testIncreasingMinutesToHours() {
    let coll = db.bucket_granularity_granularityMinutesToHours;
    let bucketsColl = db.getCollection('system.buckets.' + coll.getName());
    coll.drop();

    assert.commandWorked(db.createCollection(
        coll.getName(), {timeseries: {timeField: 't', granularity: 'minutes'}}));
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
    assert.eq(2, bucketsColl.find().itcount());

    // Expect bucket max span to be 30 days. A new measurement outside of this range should create
    // a new bucket.
    assert.commandWorked(coll.insert({t: ISODate("2021-05-23T20:00:00.001Z")}));
    assert.eq(3, bucketsColl.find().itcount());

    // Make sure when we query, we use the new bucket max span to make sure we get all matches
    assert.eq(4, coll.find({t: {$gt: ISODate("2021-05-22T00:00:00.000Z")}}).itcount());
})();

(function testReducingGranularityFails() {
    let coll = db.bucket_granularity_reducingGranularityFails;
    coll.drop();

    assert.commandWorked(db.createCollection(
        coll.getName(), {timeseries: {timeField: 't', granularity: 'minutes'}}));

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
})();
