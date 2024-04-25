/**
 * Tests running the update command on a time-series collection closes any in-memory buckets that
 * were updated.
 *
 * @tags: [
 *   # This test depends on certain writes ending up in the same bucket. Stepdowns and tenant
 *   # migrations may result in writes splitting between two primaries, and thus different buckets.
 *   does_not_support_stepdowns,
 *   tenant_migration_incompatible,
 *   # Test explicitly relies on multi-updates.
 *   requires_multi_updates,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   # This test depends on stats read from the primary node in replica sets.
 *   assumes_read_preference_unchanged,
 *   # TODO SERVER-89764 a concurrent moveCollection during insertion can cause the bucket
 *   # collection to insert more documents then expected by the test.
 *   assumes_balancer_off,
 * ]
 */
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";

TimeseriesTest.run((insert) => {
    const testDB = db.getSiblingDB(jsTestName());
    assert.commandWorked(testDB.dropDatabase());
    const coll = testDB["t"];
    const bucketsColl = testDB["system.buckets." + coll.getName()];

    const timeFieldName = "time";
    const metaFieldName = "m";

    const docs = [
        {[timeFieldName]: ISODate("2021-01-01T01:00:00Z"), [metaFieldName]: "a"},
        {[timeFieldName]: ISODate("2021-01-01T01:01:00Z"), [metaFieldName]: "a"},
        {[timeFieldName]: ISODate("2021-01-01T01:02:00Z"), [metaFieldName]: "b"},
    ];

    assert.commandWorked(testDB.createCollection(
        coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));

    assert.commandWorked(insert(coll, docs[0]));
    assert.commandWorked(testDB.runCommand({
        update: coll.getName(),
        updates: [{q: {[metaFieldName]: "a"}, u: {$set: {[metaFieldName]: "b"}}, multi: true}]
    }));
    docs[0][metaFieldName] = "b";

    let stats = assert.commandWorked(coll.stats());
    assert(stats.timeseries);
    const expectedNumBucketsReopened = stats.timeseries['numBucketsReopened'] + 1;

    assert.commandWorked(insert(coll, docs.slice(1)));
    assert.docEq(docs, coll.find({}, {_id: 0}).sort({[timeFieldName]: 1}).toArray());

    assert.eq(bucketsColl.find().itcount(), 2, bucketsColl.find().toArray());
    stats = assert.commandWorked(coll.stats());
    assert(stats.timeseries);
    assert.eq(stats.timeseries['bucketCount'], 2);
    assert.eq(stats.timeseries['numBucketsReopened'], expectedNumBucketsReopened);
});
