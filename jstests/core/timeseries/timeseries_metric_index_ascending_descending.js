/**
 * Tests creating and using ascending and descending indexes on time-series measurement fields.
 *
 * @tags: [
 *     assumes_no_implicit_collection_creation_after_drop,
 *     does_not_support_stepdowns,
 *     does_not_support_transactions,
 *     requires_fcv_50,
 *     requires_fcv_51,
 *     requires_find_command,
 *     requires_getmore,
 * ]
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");

if (!TimeseriesTest.timeseriesMetricIndexesEnabled(db.getMongo())) {
    jsTestLog(
        "Skipped test as the featureFlagTimeseriesMetricIndexes feature flag is not enabled.");
    return;
}

TimeseriesTest.run((insert) => {
    const collName = "timeseries_metric_index_ascending_descending";

    const timeFieldName = "tm";
    const metaFieldName = "mm";

    // Unique metadata values to create separate buckets.
    const firstDoc = {_id: 0, [timeFieldName]: ISODate(), [metaFieldName]: {tag: "a"}, x: 1};
    const secondDoc = {_id: 1, [timeFieldName]: ISODate(), [metaFieldName]: {tag: "b"}, x: 2};
    const thirdDoc = {_id: 2, [timeFieldName]: ISODate(), [metaFieldName]: {tag: "c"}, "x.y": 3};
    const fourthDoc = {_id: 3, [timeFieldName]: ISODate(), [metaFieldName]: {tag: "d"}, x: {y: 4}};

    const setup = function(keyForCreate) {
        const coll = db.getCollection(collName);
        coll.drop();

        jsTestLog("Setting up collection: " + coll.getFullName() +
                  " with index: " + tojson(keyForCreate));

        assert.commandWorked(db.createCollection(
            coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));

        // Insert data on the time-series collection and index it.
        assert.commandWorked(insert(coll, firstDoc), "failed to insert doc: " + tojson(firstDoc));
        assert.commandWorked(insert(coll, secondDoc), "failed to insert doc: " + tojson(secondDoc));
        assert.commandWorked(insert(coll, thirdDoc), "failed to insert doc: " + tojson(thirdDoc));
        assert.commandWorked(insert(coll, fourthDoc), "failed to insert doc: " + tojson(fourthDoc));
        assert.commandWorked(coll.createIndex(keyForCreate),
                             "failed to create index: " + tojson(keyForCreate));
    };

    const testHint = function(indexName) {
        const coll = db.getCollection(collName);
        const bucketsColl = db.getCollection("system.buckets." + collName);

        // Tests hint() using the index name.
        assert.eq(4, bucketsColl.find().hint(indexName).toArray().length);
        assert.eq(4, coll.find().hint(indexName).toArray().length);

        // Tests that hint() cannot be used when the index is hidden.
        assert.commandWorked(coll.hideIndex(indexName));
        assert.commandFailedWithCode(
            assert.throws(() => bucketsColl.find().hint(indexName).toArray()), ErrorCodes.BadValue);
        assert.commandFailedWithCode(assert.throws(() => coll.find().hint(indexName).toArray()),
                                                  ErrorCodes.BadValue);
        assert.commandWorked(coll.unhideIndex(indexName));
    };

    const coll = db.getCollection(collName);
    const bucketsColl = db.getCollection("system.buckets." + collName);

    // Test a simple ascending index.
    setup({x: 1});
    let indexes = bucketsColl.getIndexes();
    assert.eq(1, indexes.length);
    assert.eq({"control.max.x": 1, "control.min.x": 1}, indexes[0].key);
    testHint(indexes[0].name);

    // Drop index by key pattern.
    assert.commandWorked(coll.dropIndex({x: 1}));
    indexes = bucketsColl.getIndexes();
    assert.eq(0, indexes.length);

    // Test a simple descending index.
    setup({x: -1});
    indexes = bucketsColl.getIndexes();
    assert.eq(1, indexes.length);
    assert.eq({"control.min.x": -1, "control.max.x": -1}, indexes[0].key);
    testHint(indexes[0].name);

    // Drop index by name.
    assert.commandWorked(coll.dropIndex(indexes[0].name));
    indexes = bucketsColl.getIndexes();
    assert.eq(0, indexes.length);

    // Test an index on dotted and sub document fields.
    setup({"x.y": 1});
    indexes = bucketsColl.getIndexes();
    assert.eq(1, indexes.length);
    assert.eq({"control.max.x.y": 1, "control.min.x.y": 1}, indexes[0].key);
    testHint(indexes[0].name);

    assert.commandWorked(coll.dropIndex(indexes[0].name));
    indexes = bucketsColl.getIndexes();
    assert.eq(0, indexes.length);

    // Test bad input.
    assert.commandFailedWithCode(coll.createIndex({x: "abc"}), ErrorCodes.CannotCreateIndex);
    assert.commandFailedWithCode(coll.createIndex({x: {y: 1}}), ErrorCodes.CannotCreateIndex);
    assert.commandFailedWithCode(coll.createIndex({x: true}), ErrorCodes.CannotCreateIndex);
});
}());
