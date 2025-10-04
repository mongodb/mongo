/**
 * Tests that hashed indexes are prohibited on measurement fields.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 */
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";

TimeseriesTest.run((insert) => {
    const collName = jsTestName();

    const timeFieldName = "tm";
    const metaFieldName = "mm";

    // Unique metadata values to create separate buckets.
    const doc = {_id: 0, [timeFieldName]: ISODate(), [metaFieldName]: {tag: "a"}, x: 1};

    const testIndex = function (keysForCreate) {
        const coll = db.getCollection(collName);
        coll.drop();

        jsTestLog("Setting up collection: " + coll.getFullName() + " with index: " + tojson(keysForCreate));

        assert.commandWorked(
            db.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}),
        );

        // Insert data on the time-series collection and index it.
        assert.commandWorked(insert(coll, doc), "failed to insert doc: " + tojson(doc));
        assert.commandFailedWithCode(coll.createIndex(keysForCreate), ErrorCodes.CannotCreateIndex);
    };

    testIndex({_id: "hashed"});
    testIndex({x: "hashed"});
    testIndex({x: 1, y: "hashed"});
    testIndex({[`${metaFieldName}.tag`]: 1, x: "hashed"});
    testIndex({x: 1, [`${metaFieldName}.tag`]: -1, y: "hashed"});
});
