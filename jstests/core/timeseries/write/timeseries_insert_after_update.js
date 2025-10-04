/**
 * Tests running the update command on a time-series collection closes any in-memory buckets that
 * were updated.
 *
 * @tags: [
 *   # This test depends on certain writes ending up in the same bucket. Stepdowns and tenant
 *   # migrations may result in writes splitting between two primaries, and thus different buckets.
 *   does_not_support_stepdowns,
 *   # Test explicitly relies on multi-updates.
 *   requires_multi_updates,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   # This test depends on stats read from the primary node in replica sets.
 *   assumes_read_preference_unchanged,
 *   # This test depends on the stats tracked by UUID
 *   assumes_stable_collection_uuid,
 * ]
 */
import {getTimeseriesCollForRawOps} from "jstests/core/libs/raw_operation_utils.js";
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";

TimeseriesTest.run((insert) => {
    const testDB = db.getSiblingDB(jsTestName());
    assert.commandWorked(testDB.dropDatabase());
    const coll = testDB["t"];

    const timeFieldName = "time";
    const metaFieldName = "m";

    const docs = [
        {[timeFieldName]: ISODate("2021-01-01T01:00:00Z"), [metaFieldName]: "a"},
        {[timeFieldName]: ISODate("2021-01-01T01:01:00Z"), [metaFieldName]: "a"},
        {[timeFieldName]: ISODate("2021-01-01T01:02:00Z"), [metaFieldName]: "b"},
    ];

    assert.commandWorked(
        testDB.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}),
    );

    assert.commandWorked(insert(coll, docs[0]));
    assert.commandWorked(
        testDB.runCommand({
            update: coll.getName(),
            updates: [{q: {[metaFieldName]: "a"}, u: {$set: {[metaFieldName]: "b"}}, multi: true}],
        }),
    );
    docs[0][metaFieldName] = "b";

    let stats = assert.commandWorked(coll.stats());
    assert(stats.timeseries);
    const expectedNumBucketsReopened = stats.timeseries["numBucketsReopened"] + 1;

    assert.commandWorked(insert(coll, docs.slice(1)));
    assert.docEq(
        docs,
        coll
            .find({}, {_id: 0})
            .sort({[timeFieldName]: 1})
            .toArray(),
    );

    assert.eq(
        getTimeseriesCollForRawOps(coll).find().rawData().itcount(),
        2,
        getTimeseriesCollForRawOps(coll).find().rawData().toArray(),
    );
    stats = assert.commandWorked(coll.stats());
    assert(stats.timeseries);
    assert.eq(stats.timeseries["bucketCount"], 2);
    if (!TestData.runningWithBalancer) {
        assert.eq(stats.timeseries["numBucketsReopened"], expectedNumBucketsReopened);
    } else {
        // Retries of ShardCannotRefreshDueToLocksHeld during resharding can cause the number of
        // buckets that are reopened to be higher when the balancer is enabled.
        assert.gte(stats.timeseries["numBucketsReopened"], expectedNumBucketsReopened);
    }
});
