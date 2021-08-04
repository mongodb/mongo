/**
 * Tests creating and using compound indexes on time-series metadata and measurement fields.
 *
 * @tags: [
 *     assumes_no_implicit_collection_creation_after_drop,
 *     does_not_support_stepdowns,
 *     does_not_support_transactions,
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
            [metaFieldName]: {tag: "b", r: {s: true}, loc: [3, 3]},
            x: 2,
            z: false
        },
        {
            _id: 2,
            [timeFieldName]: ISODate(),
            [metaFieldName]: {tag: "c", "r.s": "val", loc: {type: "Point", coordinates: [3, 2]}},
            "x.y": 3,
            z: true
        },
        {
            _id: 3,
            [timeFieldName]: ISODate(),
            [metaFieldName]: {tag: "d", r: "val", loc: [1, 0]},
            x: {y: 4},
            z: false
        }
    ];

    const setup = function(keysForCreate) {
        const coll = db.getCollection(collName);
        coll.drop();

        jsTestLog("Setting up collection: " + coll.getFullName() +
                  " with index: " + tojson(keysForCreate));

        assert.commandWorked(db.createCollection(
            coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));

        // Insert data on the time-series collection and index it.
        assert.commandWorked(insert(coll, docs), "failed to insert docs: " + tojson(docs));
        assert.commandWorked(coll.createIndex(keysForCreate),
                             "failed to create index: " + tojson(keysForCreate));
    };

    const testHint = function(indexName) {
        const coll = db.getCollection(collName);
        const bucketsColl = db.getCollection("system.buckets." + collName);

        // Tests hint() using the index name.
        assert.eq(docs.length, bucketsColl.find().hint(indexName).toArray().length);
        assert.eq(docs.length, coll.find().hint(indexName).toArray().length);

        // Tests that hint() cannot be used when the index is hidden.
        assert.commandWorked(coll.hideIndex(indexName));
        assert.commandFailedWithCode(
            assert.throws(() => bucketsColl.find().hint(indexName).toArray()), ErrorCodes.BadValue);
        assert.commandFailedWithCode(assert.throws(() => coll.find().hint(indexName).toArray()),
                                                  ErrorCodes.BadValue);
        assert.commandWorked(coll.unhideIndex(indexName));
    };

    const testIndex = function(viewDefinition, bucketsDefinition) {
        const coll = db.getCollection(collName);
        const bucketsColl = db.getCollection("system.buckets." + collName);

        setup(viewDefinition);

        // Check definition on view
        let userIndexes = coll.getIndexes();
        assert.eq(1, userIndexes.length);
        assert.eq(viewDefinition, userIndexes[0].key);

        // Check definition on buckets collection
        let bucketIndexes = bucketsColl.getIndexes();
        assert.eq(1, bucketIndexes.length);
        assert.eq(bucketsDefinition, bucketIndexes[0].key);

        testHint(bucketIndexes[0].name);

        // Drop index by key pattern.
        assert.commandWorked(coll.dropIndex(viewDefinition));
        bucketIndexes = bucketsColl.getIndexes();
        assert.eq(0, bucketIndexes.length);
    };

    // Test metadata-only indexes.
    testIndex({[`${metaFieldName}.tag`]: 1, [`${metaFieldName}.r`]: 1},
              {"meta.tag": 1, "meta.r": 1});
    testIndex({[`${metaFieldName}.tag`]: 1, [`${metaFieldName}.loc`]: "2dsphere"},
              {"meta.tag": 1, "meta.loc": "2dsphere"});
    testIndex({[`${metaFieldName}.loc`]: "2dsphere", [`${metaFieldName}.tag`]: 1},
              {"meta.loc": "2dsphere", "meta.tag": 1});

    // Test measurement-only indexes.
    testIndex({x: 1, z: 1},
              {"control.max.x": 1, "control.min.x": 1, "control.max.z": 1, "control.min.z": 1});
    testIndex({x: -1, z: -1},
              {"control.min.x": -1, "control.max.x": -1, "control.min.z": -1, "control.max.z": -1});
    testIndex({x: 1, z: -1},
              {"control.max.x": 1, "control.min.x": 1, "control.min.z": -1, "control.max.z": -1});
    testIndex({x: -1, z: 1},
              {"control.min.x": -1, "control.max.x": -1, "control.max.z": 1, "control.min.z": 1});

    // Test mixed metadata and measurement indexes.
    testIndex({[`${metaFieldName}.r.s`]: 1, x: 1},
              {"meta.r.s": 1, "control.max.x": 1, "control.min.x": 1});
    testIndex({[`${metaFieldName}.r.s`]: 1, x: -1},
              {"meta.r.s": 1, "control.min.x": -1, "control.max.x": -1});
    testIndex({x: 1, [`${metaFieldName}.r.s`]: 1},
              {"control.max.x": 1, "control.min.x": 1, "meta.r.s": 1});
    testIndex({x: -1, [`${metaFieldName}.r.s`]: 1},
              {"control.min.x": -1, "control.max.x": -1, "meta.r.s": 1});
    testIndex({x: 1, [`${metaFieldName}.loc`]: "2dsphere", z: -1}, {
        "control.max.x": 1,
        "control.min.x": 1,
        "meta.loc": "2dsphere",
        "control.min.z": -1,
        "control.max.z": -1
    });

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
});
}());
