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

(function testSeconds() {
    let coll = db.bucket_timestamp_rounding_granularitySeconds;
    coll.drop();

    assert.commandWorked(db.createCollection(
        coll.getName(), {timeseries: {timeField: 't', granularity: 'seconds'}}));

    // Ensure min time is rounded down to nearest minute.
    assert.commandWorked(coll.insert({t: ISODate("2021-04-22T20:10:14.134Z")}));
    // And that going backwards, but after the rounding point, doesn't open a new bucket.
    assert.commandWorked(coll.insert({t: ISODate("2021-04-22T20:10:05.000Z")}));

    const bucketsColl = db.getCollection('system.buckets.' + coll.getName());
    const buckets = bucketsColl.find().toArray();
    assert.eq(1, buckets.length);
    assert.eq(buckets[0].control.min.t, ISODate("2021-04-22T20:10:00.000Z"));
})();

(function testMinutes() {
    let coll = db.bucket_timestamp_rounding_granularityMinutes;
    coll.drop();

    assert.commandWorked(db.createCollection(
        coll.getName(), {timeseries: {timeField: 't', granularity: 'minutes'}}));

    // Ensure min time is rounded down to nearest hour.
    assert.commandWorked(coll.insert({t: ISODate("2021-04-22T20:10:14.134Z")}));
    // And that going backwards, but after the rounding point, doesn't open a new bucket.
    assert.commandWorked(coll.insert({t: ISODate("2021-04-22T20:05:00.000Z")}));

    const bucketsColl = db.getCollection('system.buckets.' + coll.getName());
    const buckets = bucketsColl.find().toArray();
    assert.eq(1, buckets.length);
    assert.eq(buckets[0].control.min.t, ISODate("2021-04-22T20:00:00.000Z"));
})();

(function testHours() {
    let coll = db.bucket_timestamp_rounding_granularityHours;
    coll.drop();

    assert.commandWorked(
        db.createCollection(coll.getName(), {timeseries: {timeField: 't', granularity: 'hours'}}));

    // Ensure min time is rounded down to nearest day.
    assert.commandWorked(coll.insert({t: ISODate("2021-04-22T20:10:14.134Z")}));
    // And that going backwards, but after the rounding point, doesn't open a new bucket.
    assert.commandWorked(coll.insert({t: ISODate("2021-04-22T10:00:00.000Z")}));

    const bucketsColl = db.getCollection('system.buckets.' + coll.getName());
    const buckets = bucketsColl.find().toArray();
    assert.eq(1, buckets.length);
    assert.eq(buckets[0].control.min.t, ISODate("2021-04-22T00:00:00.000Z"));
})();

(function testSecondsToMinutes() {
    let coll = db.bucket_timestamp_rounding_granularitySecondsToMinutes;
    coll.drop();

    assert.commandWorked(db.createCollection(
        coll.getName(), {timeseries: {timeField: 't', granularity: 'seconds'}}));

    // Ensure min time is rounded down to nearest minute.
    assert.commandWorked(coll.insert({t: ISODate("2021-04-22T20:10:14.134Z")}));
    // And that going backwards, but after the rounding point, doesn't open a new bucket.
    assert.commandWorked(coll.insert({t: ISODate("2021-04-22T20:10:05.000Z")}));

    const bucketsColl = db.getCollection('system.buckets.' + coll.getName());
    let buckets = bucketsColl.find().toArray();
    assert.eq(1, buckets.length);
    assert.eq(buckets[0].control.min.t, ISODate("2021-04-22T20:10:00.000Z"));

    // Now let's bump to minutes and make sure we get the expected behavior
    assert.commandWorked(
        db.runCommand({collMod: coll.getName(), timeseries: {granularity: 'minutes'}}));

    // Open a new bucket and ensure min time is rounded down to nearest hour.
    assert.commandWorked(coll.insert({t: ISODate("2021-04-24T20:10:14.134Z")}));
    // And that going backwards, but after the rounding point, doesn't open another new bucket.
    assert.commandWorked(coll.insert({t: ISODate("2021-04-24T20:05:00.000Z")}));

    buckets = bucketsColl.find().toArray();
    assert.eq(2, buckets.length);
    assert.eq(buckets[1].control.min.t, ISODate("2021-04-24T20:00:00.000Z"));
})();

(function testMinutesToHours() {
    let coll = db.bucket_timestamp_rounding_granularityMinutesToHours;
    coll.drop();

    assert.commandWorked(db.createCollection(
        coll.getName(), {timeseries: {timeField: 't', granularity: 'minutes'}}));

    // Ensure min time is rounded down to nearest hour.
    assert.commandWorked(coll.insert({t: ISODate("2021-04-22T20:10:14.134Z")}));
    // And that going backwards, but after the rounding point, doesn't open a new bucket.
    assert.commandWorked(coll.insert({t: ISODate("2021-04-22T20:05:00.000Z")}));

    let bucketsColl = db.getCollection('system.buckets.' + coll.getName());
    let buckets = bucketsColl.find().toArray();
    assert.eq(1, buckets.length);
    assert.eq(buckets[0].control.min.t, ISODate("2021-04-22T20:00:00.000Z"));

    // Now let's bump to minutes and make sure we get the expected behavior
    assert.commandWorked(
        db.runCommand({collMod: coll.getName(), timeseries: {granularity: 'hours'}}));

    // Open a new bucket and ensure min time is rounded down to nearest day.
    assert.commandWorked(coll.insert({t: ISODate("2021-06-24T20:10:14.134Z")}));
    // And that going backwards, but after the rounding point, doesn't open another new bucket.
    assert.commandWorked(coll.insert({t: ISODate("2021-06-24T10:00:00.000Z")}));

    buckets = bucketsColl.find().toArray();
    assert.eq(2, buckets.length);
    assert.eq(buckets[1].control.min.t, ISODate("2021-06-24T00:00:00.000Z"));
})();
})();
