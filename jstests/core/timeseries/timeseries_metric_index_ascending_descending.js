/**
 * Tests creating and using ascending and descending indexes on time-series measurement fields.
 *
 * @tags: [
 *     does_not_support_stepdowns,
 *     does_not_support_transactions,
 *     requires_find_command,
 * ]
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");
load("jstests/libs/feature_flag_util.js");
load("jstests/libs/fixture_helpers.js");

if (!FeatureFlagUtil.isEnabled(db, "TimeseriesMetricIndexes")) {
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
        const bucketsColl = db.getCollection("system.buckets." + collName);
        coll.drop();

        jsTestLog("Setting up collection: " + coll.getFullName() +
                  " with index: " + tojson(keyForCreate));

        assert.commandWorked(db.createCollection(
            coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));

        const numUserIndexesBefore = coll.getIndexes().length;
        const numBucketIndexesBefore = bucketsColl.getIndexes().length;

        // Insert data on the time-series collection and index it.
        assert.commandWorked(insert(coll, firstDoc), "failed to insert doc: " + tojson(firstDoc));
        assert.commandWorked(insert(coll, secondDoc), "failed to insert doc: " + tojson(secondDoc));
        assert.commandWorked(insert(coll, thirdDoc), "failed to insert doc: " + tojson(thirdDoc));
        assert.commandWorked(insert(coll, fourthDoc), "failed to insert doc: " + tojson(fourthDoc));
        assert.commandWorked(coll.createIndex(keyForCreate),
                             "failed to create index: " + tojson(keyForCreate));

        assert.eq(numUserIndexesBefore + 1, coll.getIndexes().length);
        assert.eq(numBucketIndexesBefore + 1, bucketsColl.getIndexes().length);
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
    let userIndexes = coll.getIndexes();
    assert.eq({x: 1}, userIndexes[userIndexes.length - 1].key);

    let bucketIndexes = bucketsColl.getIndexes();
    assert.eq({"control.min.x": 1, "control.max.x": 1},
              bucketIndexes[bucketIndexes.length - 1].key);
    testHint(bucketIndexes[bucketIndexes.length - 1].name);

    // Drop index by key pattern.
    assert.commandWorked(coll.dropIndex({x: 1}));
    bucketIndexes = bucketsColl.getIndexes();

    // Test a simple descending index.
    setup({x: -1});
    userIndexes = coll.getIndexes();
    assert.eq({x: -1}, userIndexes[userIndexes.length - 1].key);

    bucketIndexes = bucketsColl.getIndexes();
    let bucketIndex = bucketIndexes[bucketIndexes.length - 1];
    assert.eq({"control.max.x": -1, "control.min.x": -1}, bucketIndex.key);
    testHint(bucketIndex.name);

    assert.commandWorked(coll.dropIndex(bucketIndex.name));
    bucketIndexes = bucketsColl.getIndexes();

    // Test an index on dotted and sub document fields.
    setup({"x.y": 1});
    userIndexes = coll.getIndexes();
    assert.eq({"x.y": 1}, userIndexes[userIndexes.length - 1].key);

    bucketIndexes = bucketsColl.getIndexes();
    bucketIndex = bucketIndexes[bucketIndexes.length - 1];
    assert.eq({"control.min.x.y": 1, "control.max.x.y": 1}, bucketIndex.key);
    testHint(bucketIndex.name);

    assert.commandWorked(coll.dropIndex(bucketIndex.name));
    bucketIndexes = bucketsColl.getIndexes();

    // Test bad input.
    assert.commandFailedWithCode(coll.createIndex({x: "abc"}), ErrorCodes.CannotCreateIndex);
    assert.commandFailedWithCode(coll.createIndex({x: {y: 1}}), ErrorCodes.CannotCreateIndex);
    assert.commandFailedWithCode(coll.createIndex({x: true}), ErrorCodes.CannotCreateIndex);

    // Test bad data.
    assert.commandWorked(coll.createIndex({"x.y": 1}));
    const docsWithArrays = [
        {_id: 4, [timeFieldName]: ISODate(), [metaFieldName]: {tag: "d"}, x: {y: [true]}},
        {_id: 5, [timeFieldName]: ISODate(), [metaFieldName]: {tag: "d"}, x: [{y: false}]}
    ];
    assert.commandFailedWithCode(coll.insert(docsWithArrays[0]), 5930501);
    assert.commandFailedWithCode(coll.insert(docsWithArrays[1]), 5930501);
    assert.commandWorked(coll.dropIndex({"x.y": 1}));

    // Create indexes on the buckets collection that do not map to any user indexes. The server must
    // not crash when handling the reverse mapping of these.
    assert.commandWorked(bucketsColl.createIndex({"control.min.x.y": 1}));
    assert.commandWorked(bucketsColl.createIndex({"control.min.x.y": 1, "control.min.y.x": 1}));
    assert.commandWorked(bucketsColl.createIndex({"control.max.x.y": 1}));
    assert.commandWorked(bucketsColl.createIndex({"control.max.x.y": 1, "control.max.y.x": 1}));
    assert.commandWorked(bucketsColl.createIndex({"control.max.x.y": 1, "control.min.x.y": 1}));
    assert.commandWorked(bucketsColl.createIndex({"control.min.x.y": -1, "control.max.x.y": 1}));
    assert.commandWorked(bucketsColl.createIndex({"control.min.x.y": -1, "control.max.x.y": -1}));
    assert.commandWorked(bucketsColl.createIndex({"control.max.x.y": 1, "control.min.x.y": -1}));

    assert.commandWorked(bucketsColl.createIndex({"data.x": 1}));
    assert.commandWorked(bucketsColl.createIndex({"control.min.x.y": 1, "data.x": 1}));
    assert.commandWorked(bucketsColl.createIndex({"data.x": 1, "control.min.x.y": 1}));

    // The first two-thirds of the below compound indexes represent {"x.y" : 1} and {"x.y" : -1}.
    assert.commandWorked(
        bucketsColl.createIndex({"control.min.x.y": 1, "control.max.x.y": 1, "data.x": 1}));
    assert.commandWorked(
        bucketsColl.createIndex({"control.max.x.y": -1, "control.min.x.y": -1, "data.x": 1}));

    // There are more indexes for sharded collections because it includes the shard key index. When
    // time-series scalability improvements are enabled, the {meta: 1, time: 1} index gets built by
    // default on the time-series bucket collection.
    const numExtraIndexes = (FixtureHelpers.isSharded(bucketsColl) ? 1 : 0) +
        (FeatureFlagUtil.isEnabled(db, "TimeseriesScalabilityImprovements") ? 1 : 0);

    userIndexes = coll.getIndexes();
    assert.eq(numExtraIndexes, userIndexes.length);
    bucketIndexes = bucketsColl.getIndexes();
    assert.eq(13 + numExtraIndexes, bucketIndexes.length);
});
}());
