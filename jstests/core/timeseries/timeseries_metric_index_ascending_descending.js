/**
 * Tests creating and using ascending and descending indexes on time-series measurement fields.
 *
 * @tags: [
 *   # This test makes assertions on listIndexes and on the order of the indexes returned.
 *   assumes_no_implicit_index_creation,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 */
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

TimeseriesTest.run((insert) => {
    const collName = jsTestName();

    const timeFieldName = "tm";
    const metaFieldName = "mm";

    // Unique metadata values to create separate buckets.
    const docs = [
        {_id: 0, [timeFieldName]: ISODate(), [metaFieldName]: {tag: "a"}, x: 1},
        {_id: 1, [timeFieldName]: ISODate(), [metaFieldName]: {tag: "b"}, x: 2},
        {_id: 2, [timeFieldName]: ISODate(), [metaFieldName]: {tag: "c"}, "x.y": 3},
        {_id: 3, [timeFieldName]: ISODate(), [metaFieldName]: {tag: "d"}, x: {y: 4}}
    ];

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
        assert.commandWorked(insert(coll, docs), "failed to insert docs: " + tojson(docs));
        assert.commandWorked(coll.createIndex(keyForCreate),
                             "failed to create index: " + tojson(keyForCreate));

        assert.eq(numUserIndexesBefore + 1, coll.getIndexes().length);
        assert.eq(numBucketIndexesBefore + 1, bucketsColl.getIndexes().length);
    };

    const coll = db.getCollection(collName);
    const bucketsColl = db.getCollection("system.buckets." + collName);

    const testIndex = (userKeyPattern, bucketsKeyPattern) => {
        setup(userKeyPattern);
        TimeseriesTest.checkIndex(coll, userKeyPattern, bucketsKeyPattern, docs.length);
        assert.commandWorked(coll.dropIndex(userKeyPattern));
    };

    // Test a simple ascending index.
    testIndex({x: 1}, {["control.min.x"]: 1, ["control.max.x"]: 1});

    // Test a simple descending index.
    testIndex({x: -1}, {["control.max.x"]: -1, ["control.min.x"]: -1});

    // Test an index on dotted and sub document fields.
    testIndex({"x.y": 1}, {["control.min.x.y"]: 1, ["control.max.x.y"]: 1});

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
    const numExtraIndexes = (FixtureHelpers.isSharded(bucketsColl) ? 1 : 0) + 1;

    const userIndexes = coll.getIndexes();
    assert.eq(numExtraIndexes, userIndexes.length, tojson(userIndexes));
    const bucketIndexes = bucketsColl.getIndexes();
    assert.eq(13 + numExtraIndexes, bucketIndexes.length, tojson(bucketIndexes));
});
