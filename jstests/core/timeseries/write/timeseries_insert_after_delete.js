/**
 * Tests running the delete command on a time-series collection closes the in-memory bucket.
 * @tags: [
 *   # This test depends on certain writes ending up in the same bucket. Stepdowns and tenant
 *   # migrations may result in writes splitting between two primaries, and thus different buckets.
 *   does_not_support_stepdowns,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 */
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";

TimeseriesTest.run((insert) => {
    const testDB = db.getSiblingDB(jsTestName());
    assert.commandWorked(testDB.dropDatabase());
    const coll = testDB.getCollection("t");
    const timeFieldName = "time";
    const metaFieldName = "tag";

    const objA = {[timeFieldName]: ISODate("2021-01-01T01:00:00Z"), [metaFieldName]: "A"};
    const objB = {[timeFieldName]: ISODate("2021-01-01T01:01:00Z"), [metaFieldName]: "A"};

    assert.commandWorked(
        testDB.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}),
    );

    assert.commandWorked(insert(coll, objA));
    assert.eq(
        assert.commandWorked(
            testDB.runCommand({delete: coll.getName(), deletes: [{q: {[metaFieldName]: "A"}, limit: 0}]}),
        )["n"],
        1,
    );
    assert.commandWorked(insert(coll, [objB]));
    const docs = coll.find({}, {_id: 0}).toArray();
    assert.docEq([objB], docs);
    assert(coll.drop());
});
