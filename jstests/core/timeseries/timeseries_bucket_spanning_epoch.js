/**
 * Tests correctness of time-series bucketing when measurements cross the Unix Epoch and other
 * interesting boundaries.
 *
 * @tags: [
 *   # This test depends on certain writes ending up in the same bucket. Stepdowns may result in
 *   # writes splitting between two primaries, and thus different buckets.
 *   does_not_support_stepdowns,
 *   # Bucketing behavior with timestamp offsets greater than 32 bits was fixed in 6.1
 *   requires_fcv_61,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 */

(function() {

// Test that measurements spanning the Unix Epoch end up in the same bucket.
(function testUnixEpoch() {
    let coll = db.timeseries_bucket_spanning_epoch;
    let bucketsColl = db.getCollection('system.buckets.' + coll.getName());
    coll.drop();

    assert.commandWorked(db.createCollection(
        coll.getName(), {timeseries: {timeField: 't', metaField: 'm', granularity: 'hours'}}));

    // All measurements land in the same bucket.
    assert.commandWorked(coll.insert({m: 1, t: ISODate("1969-12-31T23:00:00.000Z")}));
    assert.commandWorked(coll.insert({m: 1, t: ISODate("1970-01-01T01:00:00.000Z")}));
    assert.eq(1, bucketsColl.find().itcount());
})();

// Test that measurements spanning multiples of the Unix Epoch width end up in the different buckets
(function testUnixEpoch() {
    let coll = db.timeseries_bucket_spanning_epoch;
    let bucketsColl = db.getCollection('system.buckets.' + coll.getName());
    coll.drop();

    assert.commandWorked(db.createCollection(
        coll.getName(), {timeseries: {timeField: 't', metaField: 'm', granularity: 'hours'}}));

    // Measurements land in same buckets.
    assert.commandWorked(coll.insert({m: 1, t: ISODate("1900-01-01T00:00:00.000Z")}));
    assert.commandWorked(coll.insert({m: 1, t: ISODate("1900-01-01T01:00:00.000Z")}));
    assert.eq(1, bucketsColl.find().itcount());

    // Measurements land in different buckets.
    assert.commandWorked(coll.insert({m: 1, t: ISODate("1970-01-01T01:00:00.000Z")}));
    assert.eq(2, bucketsColl.find().itcount());

    // Measurements land in different buckets.
    assert.commandWorked(coll.insert({m: 1, t: ISODate("2110-01-01T00:00:00.000Z")}));
    assert.commandWorked(coll.insert({m: 1, t: ISODate("2110-01-01T01:00:00.000Z")}));
    assert.eq(3, bucketsColl.find().itcount());
})();

// Test that measurements with timestamps equivalent modulo 2^32 end up in the same bucket.
(function testUnixEpochPlus32BitsOverflow() {
    let coll = db.timeseries_bucket_spanning_epoch;
    let bucketsColl = db.getCollection('system.buckets.' + coll.getName());
    coll.drop();

    assert.commandWorked(db.createCollection(
        coll.getName(), {timeseries: {timeField: 't', metaField: 'm', granularity: 'hours'}}));

    // Measurements land in different buckets.
    assert.commandWorked(coll.insert({m: 2, t: ISODate("2106-07-02T06:28:16.000Z")}));
    assert.commandWorked(coll.insert({m: 2, t: ISODate("1970-01-01T00:00:00.000Z")}));
    assert.eq(2, bucketsColl.find().itcount());
})();

// Test that measurements with a difference of more than the maximum time span expressible in 32-bit
// precision seconds-count cannot overflow to end up in the same bucket.
(function testUnixEpochPlus32BitsAndSomeOverflow() {
    let coll = db.timeseries_bucket_spanning_epoch;
    let bucketsColl = db.getCollection('system.buckets.' + coll.getName());
    coll.drop();

    assert.commandWorked(db.createCollection(
        coll.getName(), {timeseries: {timeField: 't', metaField: 'm', granularity: 'hours'}}));

    // Measurements land in different buckets.
    assert.commandWorked(coll.insert({m: 2, t: ISODate("2105-06-24T06:28:16Z")}));
    assert.commandWorked(coll.insert({m: 2, t: ISODate("1969-05-18T00:00:00.000Z")}));
    assert.eq(2, bucketsColl.find().itcount());
})();
})();
