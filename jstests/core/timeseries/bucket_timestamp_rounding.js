/**
 * Tests correctness of time-series bucket granularity configuration.
 *
 * @tags: [
 *   # This test depends on certain writes ending up in the same bucket. Stepdowns and tenant
 *   # migrations may result in writes splitting between two primaries, and thus different buckets.
 *   does_not_support_stepdowns,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 */

(function testSeconds() {
    let coll = db[jsTestName() + '_granularitySeconds'];
    coll.drop();

    assert.commandWorked(db.createCollection(
        coll.getName(), {timeseries: {timeField: 't', granularity: 'seconds'}}));
    if (TestData.runningWithBalancer) {
        assert.commandWorked(coll.createIndex({'t': 1}));
    }

    // Ensure min time is rounded down to nearest bucketRoundingSeconds (minute).
    assert.commandWorked(coll.insert({t: ISODate("2021-04-22T20:10:14.134Z")}));
    // And that going backwards, but after the rounding point, doesn't open a new bucket.
    assert.commandWorked(coll.insert({t: ISODate("2021-04-22T20:10:05.000Z")}));
    // And reaching the boundary of bucketMaxSpanSeconds (1 hour) above the control.min time also
    // doesn't open a new bucket.
    assert.commandWorked(coll.insert({t: ISODate("2021-04-22T21:09:59.999Z")}));

    const bucketsColl = db.getCollection('system.buckets.' + coll.getName());
    let buckets = bucketsColl.find().toArray();
    assert.eq(1, buckets.length);
    assert.eq(buckets[0].control.min.t, ISODate("2021-04-22T20:10:00.000Z"));
    // The timestamp component of the bucketId should equal the min time field.
    assert.eq(buckets[0].control.min.t, buckets[0]._id.getTimestamp());

    // Exceeding the bucket boundary should create a new bucket.
    assert.commandWorked(coll.insert({t: ISODate("2021-04-22T21:10:00.000Z")}));
    buckets = bucketsColl.find().toArray();
    assert.eq(2, buckets.length);
})();

(function testMinutes() {
    let coll = db[jsTestName() + '_granularityMinutes'];
    coll.drop();

    assert.commandWorked(db.createCollection(
        coll.getName(), {timeseries: {timeField: 't', granularity: 'minutes'}}));
    if (TestData.runningWithBalancer) {
        assert.commandWorked(coll.createIndex({'t': 1}));
    }

    // Ensure min time is rounded down to nearest bucketRoundingSeconds (hour).
    assert.commandWorked(coll.insert({t: ISODate("2021-04-22T20:10:14.134Z")}));
    // And that going backwards, but after the rounding point, doesn't open a new bucket.
    assert.commandWorked(coll.insert({t: ISODate("2021-04-22T20:05:00.000Z")}));
    // And reaching the boundary of bucketMaxSpanSeconds (1 day) above the control.min time also
    // doesn't open a new bucket.
    assert.commandWorked(coll.insert({t: ISODate("2021-04-23T19:59:59.999Z")}));

    const bucketsColl = db.getCollection('system.buckets.' + coll.getName());
    let buckets = bucketsColl.find().toArray();
    assert.eq(1, buckets.length);
    assert.eq(buckets[0].control.min.t, ISODate("2021-04-22T20:00:00.000Z"));
    // The timestamp component of the bucketId should equal the min time field.
    assert.eq(buckets[0].control.min.t, buckets[0]._id.getTimestamp());

    // Exceeding the bucket boundary should create a new bucket.
    assert.commandWorked(coll.insert({t: ISODate("2021-04-23T20:00:00.000Z")}));
    buckets = bucketsColl.find().toArray();
    assert.eq(2, buckets.length);
})();

(function testHours() {
    let coll = db[jsTestName() + '_granularityHours'];
    coll.drop();

    assert.commandWorked(
        db.createCollection(coll.getName(), {timeseries: {timeField: 't', granularity: 'hours'}}));
    if (TestData.runningWithBalancer) {
        assert.commandWorked(coll.createIndex({'t': 1}));
    }

    // Ensure min time is rounded down to nearest bucketRoundingSeconds (day).
    assert.commandWorked(coll.insert({t: ISODate("2021-04-22T20:10:14.134Z")}));
    // And that going backwards, but after the rounding point, doesn't open a new bucket.
    assert.commandWorked(coll.insert({t: ISODate("2021-04-22T10:00:00.000Z")}));
    // And reaching the boundary of bucketMaxSpanSeconds (30 days) above the control.min time also
    // doesn't open a new bucket.
    assert.commandWorked(coll.insert({t: ISODate("2021-05-21T23:59:59.999Z")}));

    const bucketsColl = db.getCollection('system.buckets.' + coll.getName());
    let buckets = bucketsColl.find().toArray();
    jsTestLog(tojson(buckets));
    assert.eq(1, buckets.length);
    assert.eq(buckets[0].control.min.t, ISODate("2021-04-22T00:00:00.000Z"));
    // The timestamp component of the bucketId should equal the min time field.
    assert.eq(buckets[0].control.min.t, buckets[0]._id.getTimestamp());

    // Exceeding the bucket boundary should create a new bucket.
    assert.commandWorked(coll.insert({t: ISODate("2021-05-23T00:00:00.000Z")}));
    buckets = bucketsColl.find().toArray();
    assert.eq(2, buckets.length);
})();

(function testSecondsToMinutes() {
    let coll = db[jsTestName() + '_granularitySecondsToMinutes'];
    coll.drop();

    assert.commandWorked(db.createCollection(
        coll.getName(), {timeseries: {timeField: 't', granularity: 'seconds'}}));
    if (TestData.runningWithBalancer) {
        assert.commandWorked(coll.createIndex({'t': 1}));
    }

    // Ensure min time is rounded down to nearest bucketRoundingSeconds (minute).
    assert.commandWorked(coll.insert({t: ISODate("2021-04-22T20:10:14.134Z")}));
    // And that going backwards, but after the rounding point, doesn't open a new bucket.
    assert.commandWorked(coll.insert({t: ISODate("2021-04-22T20:10:05.000Z")}));
    // And reaching the boundary of bucketMaxSpanSeconds (1 hour) above the control.min time also
    // doesn't open a new bucket.
    assert.commandWorked(coll.insert({t: ISODate("2021-04-22T21:09:59.999Z")}));

    const bucketsColl = db.getCollection('system.buckets.' + coll.getName());
    let buckets = bucketsColl.find().toArray();
    assert.eq(1, buckets.length);
    assert.eq(buckets[0].control.min.t, ISODate("2021-04-22T20:10:00.000Z"));
    // The timestamp component of the bucketId should equal the min time field.
    assert.eq(buckets[0].control.min.t, buckets[0]._id.getTimestamp());

    // Now let's bump to minutes and make sure we get the expected behavior
    assert.commandWorked(
        db.runCommand({collMod: coll.getName(), timeseries: {granularity: 'minutes'}}));

    // Open a new bucket and ensure min time is rounded down to nearest bucketRoundingSeconds
    // (hour).
    assert.commandWorked(coll.insert({t: ISODate("2021-04-24T20:10:14.134Z")}));
    // And that going backwards, but after the rounding point, doesn't open another new bucket.
    assert.commandWorked(coll.insert({t: ISODate("2021-04-24T20:05:00.000Z")}));
    // And reaching the boundary of bucketMaxSpanSeconds (1 day) above the control.min time also
    // doesn't open a new bucket.
    assert.commandWorked(coll.insert({t: ISODate("2021-04-25T19:59:59.999Z")}));

    buckets = bucketsColl.find().toArray();
    assert.eq(2, buckets.length);
    assert.eq(buckets[1].control.min.t, ISODate("2021-04-24T20:00:00.000Z"));
    // The timestamp component of the bucketId should equal the min time field.
    assert.eq(buckets[1].control.min.t, buckets[1]._id.getTimestamp());

    // Exceeding the bucket boundary should create a new bucket.
    assert.commandWorked(coll.insert({t: ISODate("2021-04-26T00:00:00.000Z")}));
    buckets = bucketsColl.find().toArray();
    assert.eq(3, buckets.length);
})();

(function testMinutesToHours() {
    let coll = db[jsTestName() + '_granularityMinutesToHours'];
    coll.drop();

    assert.commandWorked(db.createCollection(
        coll.getName(), {timeseries: {timeField: 't', granularity: 'minutes'}}));
    if (TestData.runningWithBalancer) {
        assert.commandWorked(coll.createIndex({'t': 1}));
    }

    // Ensure min time is rounded down to nearest bucketRoundingSeconds (hour).
    assert.commandWorked(coll.insert({t: ISODate("2021-04-22T20:10:14.134Z")}));
    // And that going backwards, but after the rounding point, doesn't open a new bucket.
    assert.commandWorked(coll.insert({t: ISODate("2021-04-22T20:05:00.000Z")}));
    // And reaching the boundary of bucketMaxSpanSeconds (1 day) above the control.min time also
    // doesn't open a new bucket.
    assert.commandWorked(coll.insert({t: ISODate("2021-04-22T21:09:59.999Z")}));

    let bucketsColl = db.getCollection('system.buckets.' + coll.getName());
    let buckets = bucketsColl.find().toArray();
    assert.eq(1, buckets.length);
    assert.eq(buckets[0].control.min.t, ISODate("2021-04-22T20:00:00.000Z"));
    // The timestamp component of the bucketId should equal the min time field.
    assert.eq(buckets[0].control.min.t, buckets[0]._id.getTimestamp());

    // Now let's bump to minutes and make sure we get the expected behavior
    assert.commandWorked(
        db.runCommand({collMod: coll.getName(), timeseries: {granularity: 'hours'}}));

    // Open a new bucket and ensure min time is rounded down to nearest bucketRoundingSeconds (day).
    assert.commandWorked(coll.insert({t: ISODate("2021-06-24T20:10:14.134Z")}));
    // And that going backwards, but after the rounding point, doesn't open another new bucket.
    assert.commandWorked(coll.insert({t: ISODate("2021-06-24T10:00:00.000Z")}));
    // And reaching the boundary of bucketMaxSpanSeconds (30 days) above the control.min time also
    // doesn't open a new bucket.
    assert.commandWorked(coll.insert({t: ISODate("2021-07-23T23:59:59.999Z")}));

    buckets = bucketsColl.find().toArray();
    assert.eq(2, buckets.length);
    assert.eq(buckets[1].control.min.t, ISODate("2021-06-24T00:00:00.000Z"));
    // The timestamp component of the bucketId should equal the min time field.
    assert.eq(buckets[1].control.min.t, buckets[1]._id.getTimestamp());

    // Exceeding the bucket boundary should create a new bucket.
    assert.commandWorked(coll.insert({t: ISODate("2021-07-25T00:00:00.000Z")}));
    buckets = bucketsColl.find().toArray();
    assert.eq(3, buckets.length);
})();
