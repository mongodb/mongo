/**
 * Tests correctness of time-series bucket granularity configuration.
 *
 * @tags: [
 *   # TODO SERVER-57570: Remove this tag.
 *   assumes_against_mongod_not_mongos,
 *   assumes_no_implicit_collection_creation_after_drop,
 *   assumes_unsharded_collection,
 *   # This test depends on certain writes ending up in the same bucket. Stepdowns may result in
 *   # writes splitting between two primaries, and thus different buckets.
 *   does_not_support_stepdowns,
 *   # Same goes for tenant migrations.
 *   tenant_migration_incompatible,
 *   does_not_support_transactions,
 *   requires_fcv_49,
 *   requires_find_command,
 *   requires_timeseries,
 * ]
 */

(function() {

(function testSeconds() {
    let coll = db.granularitySeconds;
    coll.drop();

    assert.commandWorked(db.createCollection(
        coll.getName(), {timeseries: {timeField: 't', granularity: 'seconds'}}));

    // Ensure min time is rounded down to nearest minute.
    assert.commandWorked(coll.insert({t: ISODate("2021-04-22T20:10:14.134Z")}));
    // And that going backwards, but after the rounding point, doesn't open a new bucket.
    assert.commandWorked(coll.insert({t: ISODate("2021-04-22T20:10:05.000Z")}));

    const buckets = db.system.buckets.granularitySeconds.find().toArray();
    assert.eq(1, buckets.length);
    assert.eq(buckets[0].control.min.t, ISODate("2021-04-22T20:10:00.000Z"));
})();

(function testMinutes() {
    let coll = db.granularityMinutes;
    coll.drop();

    assert.commandWorked(db.createCollection(
        coll.getName(), {timeseries: {timeField: 't', granularity: 'minutes'}}));

    // Ensure min time is rounded down to nearest hour.
    assert.commandWorked(coll.insert({t: ISODate("2021-04-22T20:10:14.134Z")}));
    // And that going backwards, but after the rounding point, doesn't open a new bucket.
    assert.commandWorked(coll.insert({t: ISODate("2021-04-22T20:05:00.000Z")}));

    const buckets = db.system.buckets.granularityMinutes.find().toArray();
    assert.eq(1, buckets.length);
    assert.eq(buckets[0].control.min.t, ISODate("2021-04-22T20:00:00.000Z"));
})();

(function testHours() {
    let coll = db.granularityHours;
    coll.drop();

    assert.commandWorked(
        db.createCollection(coll.getName(), {timeseries: {timeField: 't', granularity: 'hours'}}));

    // Ensure min time is rounded down to nearest day.
    assert.commandWorked(coll.insert({t: ISODate("2021-04-22T20:10:14.134Z")}));
    // And that going backwards, but after the rounding point, doesn't open a new bucket.
    assert.commandWorked(coll.insert({t: ISODate("2021-04-22T10:00:00.000Z")}));

    const buckets = db.system.buckets.granularityHours.find().toArray();
    assert.eq(1, buckets.length);
    assert.eq(buckets[0].control.min.t, ISODate("2021-04-22T00:00:00.000Z"));
})();

(function testSecondsToMinutes() {
    let coll = db.granularitySecondsToMinutes;
    coll.drop();

    assert.commandWorked(db.createCollection(
        coll.getName(), {timeseries: {timeField: 't', granularity: 'seconds'}}));

    // Ensure min time is rounded down to nearest minute.
    assert.commandWorked(coll.insert({t: ISODate("2021-04-22T20:10:14.134Z")}));
    // And that going backwards, but after the rounding point, doesn't open a new bucket.
    assert.commandWorked(coll.insert({t: ISODate("2021-04-22T20:10:05.000Z")}));

    let buckets = db.system.buckets.granularitySecondsToMinutes.find().toArray();
    assert.eq(1, buckets.length);
    assert.eq(buckets[0].control.min.t, ISODate("2021-04-22T20:10:00.000Z"));

    // Now let's bump to minutes and make sure we get the expected behavior
    assert.commandWorked(
        db.runCommand({collMod: coll.getName(), timeseries: {granularity: 'minutes'}}));

    // Open a new bucket and ensure min time is rounded down to nearest hour.
    assert.commandWorked(coll.insert({t: ISODate("2021-04-24T20:10:14.134Z")}));
    // And that going backwards, but after the rounding point, doesn't open another new bucket.
    assert.commandWorked(coll.insert({t: ISODate("2021-04-24T20:05:00.000Z")}));

    buckets = db.system.buckets.granularitySecondsToMinutes.find().toArray();
    assert.eq(2, buckets.length);
    assert.eq(buckets[1].control.min.t, ISODate("2021-04-24T20:00:00.000Z"));
})();

(function testMinutesToHours() {
    let coll = db.granularityMinutesToHours;
    coll.drop();

    assert.commandWorked(db.createCollection(
        coll.getName(), {timeseries: {timeField: 't', granularity: 'minutes'}}));

    // Ensure min time is rounded down to nearest hour.
    assert.commandWorked(coll.insert({t: ISODate("2021-04-22T20:10:14.134Z")}));
    // And that going backwards, but after the rounding point, doesn't open a new bucket.
    assert.commandWorked(coll.insert({t: ISODate("2021-04-22T20:05:00.000Z")}));

    let buckets = db.system.buckets.granularityMinutesToHours.find().toArray();
    assert.eq(1, buckets.length);
    assert.eq(buckets[0].control.min.t, ISODate("2021-04-22T20:00:00.000Z"));

    // Now let's bump to minutes and make sure we get the expected behavior
    assert.commandWorked(
        db.runCommand({collMod: coll.getName(), timeseries: {granularity: 'hours'}}));

    // Open a new bucket and ensure min time is rounded down to nearest day.
    assert.commandWorked(coll.insert({t: ISODate("2021-06-24T20:10:14.134Z")}));
    // And that going backwards, but after the rounding point, doesn't open another new bucket.
    assert.commandWorked(coll.insert({t: ISODate("2021-06-24T10:00:00.000Z")}));

    buckets = db.system.buckets.granularityMinutesToHours.find().toArray();
    assert.eq(2, buckets.length);
    assert.eq(buckets[1].control.min.t, ISODate("2021-06-24T00:00:00.000Z"));
})();
})();
