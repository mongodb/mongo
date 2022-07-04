/**
 * Tests creating and using compound indexes on time-series metadata and measurement fields.
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

if (!FeatureFlagUtil.isEnabled(db, "TimeseriesMetricIndexes")) {
    jsTestLog(
        "Skipped test as the featureFlagTimeseriesMetricIndexes feature flag is not enabled.");
    return;
}

TimeseriesTest.run((insert) => {
    const collName = "timeseries_metric_index_compound";

    const timeFieldName = "tm";
    const metaFieldName = "mm";

    // Unique metadata values to create separate buckets.
    const docs = [
        {
            _id: 0,
            [timeFieldName]: ISODate(),
            [metaFieldName]: {tag: "a", r: 1, loc: {type: "Point", coordinates: [3, 3]}},
            x: 1,
            z: true
        },
        {
            _id: 1,
            [timeFieldName]: ISODate(),
            [metaFieldName]: {tag: "b", r: {s: true}, loc2: [3, 3]},
            x: 2,
            z: false
        },
        {
            _id: 2,
            [timeFieldName]: ISODate(),
            [metaFieldName]: {tag: "c", "r.s": "val", loc: {type: "Point", coordinates: [3, 2]}},
            a: [1, 2],
            "x.y": 3,
            z: true
        },
        {
            _id: 3,
            [timeFieldName]: ISODate(),
            [metaFieldName]: {tag: "d", r: "val", loc2: [1, 0]},
            x: {y: 4},
            z: false
        }
    ];

    const setup = function(keysForCreate) {
        const coll = db.getCollection(collName);
        const bucketsColl = db.getCollection("system.buckets." + collName);
        coll.drop();

        jsTestLog("Setting up collection: " + coll.getFullName() +
                  " with index: " + tojson(keysForCreate));

        assert.commandWorked(db.createCollection(
            coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));

        const numUserIndexesBefore = coll.getIndexes().length;
        const numBucketIndexesBefore = bucketsColl.getIndexes().length;

        // Insert data on the time-series collection and index it.
        assert.commandWorked(insert(coll, docs), "failed to insert docs: " + tojson(docs));
        assert.commandWorked(coll.createIndex(keysForCreate),
                             "failed to create index: " + tojson(keysForCreate));

        assert.eq(numUserIndexesBefore + 1, coll.getIndexes().length);
        assert.eq(numBucketIndexesBefore + 1, bucketsColl.getIndexes().length);
    };

    const testHint = function(indexName, count) {
        const coll = db.getCollection(collName);
        const bucketsColl = db.getCollection("system.buckets." + collName);

        // Tests hint() using the index name.
        assert.eq(count, bucketsColl.find().hint(indexName).toArray().length);
        assert.eq(count, coll.find().hint(indexName).toArray().length);

        // Tests that hint() cannot be used when the index is hidden.
        assert.commandWorked(coll.hideIndex(indexName));
        assert.commandFailedWithCode(
            assert.throws(() => bucketsColl.find().hint(indexName).toArray()), ErrorCodes.BadValue);
        assert.commandFailedWithCode(assert.throws(() => coll.find().hint(indexName).toArray()),
                                                  ErrorCodes.BadValue);
        assert.commandWorked(coll.unhideIndex(indexName));
    };

    const testIndex = function(viewDefinition, bucketsDefinition, count) {
        if (count === undefined) {
            count = docs.length;
        }

        const coll = db.getCollection(collName);
        const bucketsColl = db.getCollection("system.buckets." + collName);

        setup(viewDefinition);

        // Check definition on view
        let userIndexes = coll.getIndexes();
        assert.eq(viewDefinition, userIndexes[userIndexes.length - 1].key);

        // Check definition on buckets collection
        let bucketIndexes = bucketsColl.getIndexes();
        assert.eq(bucketsDefinition, bucketIndexes[bucketIndexes.length - 1].key);

        testHint(bucketIndexes[bucketIndexes.length - 1].name, count);

        // Drop index by key pattern.
        assert.commandWorked(coll.dropIndex(viewDefinition));
    };

    // Test metadata-only indexes.
    testIndex({[`${metaFieldName}.tag`]: 1, [`${metaFieldName}.r`]: 1},
              {"meta.tag": 1, "meta.r": 1});
    testIndex({[`${metaFieldName}.tag`]: 1, [`${metaFieldName}.loc`]: "2dsphere"},
              {"meta.tag": 1, "meta.loc": "2dsphere"},
              2);
    testIndex({[`${metaFieldName}.loc`]: "2dsphere", [`${metaFieldName}.tag`]: 1},
              {"meta.loc": "2dsphere", "meta.tag": 1},
              2);

    // Test measurement-only indexes.
    testIndex({x: 1, z: 1},
              {"control.min.x": 1, "control.max.x": 1, "control.min.z": 1, "control.max.z": 1});
    testIndex({x: -1, z: -1},
              {"control.max.x": -1, "control.min.x": -1, "control.max.z": -1, "control.min.z": -1});
    testIndex({x: 1, z: -1},
              {"control.min.x": 1, "control.max.x": 1, "control.max.z": -1, "control.min.z": -1});
    testIndex({x: -1, z: 1},
              {"control.max.x": -1, "control.min.x": -1, "control.min.z": 1, "control.max.z": 1});

    // Test mixed metadata and measurement indexes.
    testIndex({[`${metaFieldName}.r.s`]: 1, x: 1},
              {"meta.r.s": 1, "control.min.x": 1, "control.max.x": 1});
    testIndex({[`${metaFieldName}.r.s`]: 1, x: -1},
              {"meta.r.s": 1, "control.max.x": -1, "control.min.x": -1});
    testIndex({x: 1, [`${metaFieldName}.r.s`]: 1},
              {"control.min.x": 1, "control.max.x": 1, "meta.r.s": 1});
    testIndex({x: -1, [`${metaFieldName}.r.s`]: 1},
              {"control.max.x": -1, "control.min.x": -1, "meta.r.s": 1});
    testIndex({x: 1, [`${metaFieldName}.loc`]: "2dsphere", z: -1},
              {
                  "control.min.x": 1,
                  "control.max.x": 1,
                  "meta.loc": "2dsphere",
                  "control.max.z": -1,
                  "control.min.z": -1
              },
              2);
    testIndex({[`${metaFieldName}.loc2`]: "2d", x: 1, z: -1},
              {
                  "meta.loc2": "2d",
                  "control.min.x": 1,
                  "control.max.x": 1,
                  "control.max.z": -1,
                  "control.min.z": -1
              },
              2);

    // Test bad input.
    const testBadIndex = function(keysForCreate) {
        const coll = db.getCollection(collName);
        coll.drop();

        assert.commandWorked(db.createCollection(
            coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));

        assert.commandFailedWithCode(coll.createIndex(keysForCreate), ErrorCodes.CannotCreateIndex);
    };

    testBadIndex({x: 1, z: "abc"});
    testBadIndex({x: 1, z: {y: 1}});
    testBadIndex({x: true, z: 1});

    // Check that array-valued measurements aren't accepted.
    const testBadIndexForData = function(keysForCreate) {
        const coll = db.getCollection(collName);
        coll.drop();

        assert.commandWorked(db.createCollection(
            coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));
        assert.commandWorked(coll.createIndex(keysForCreate));

        assert.commandFailedWithCode(coll.insert(docs), 5930501);
    };

    testBadIndexForData({x: 1, a: 1});
    testBadIndexForData({[metaFieldName + ".loc"]: "2dsphere", a: 1});
    testBadIndexForData({[metaFieldName + ".loc2"]: "2d", a: 1});
    testBadIndexForData({[metaFieldName + ".r"]: "hashed", a: 1});
});
}());
