/**
 * Tests that the original user index definition is stored on the transformed index definition on
 * the buckets collection for newly supported index types. Indexes created directly on the buckets
 * collection do not have an original user index definition and rely on the reverse mapping
 * mechanism.
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

TimeseriesTest.run(() => {
    const collName = "timeseries_index_spec";

    const timeFieldName = "tm";
    const metaFieldName = "mm";

    const coll = db.getCollection(collName);
    const bucketsColl = db.getCollection("system.buckets." + collName);
    coll.drop();

    assert.commandWorked(db.createCollection(
        coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));

    assert.commandWorked(coll.createIndex({[timeFieldName]: 1}));
    assert.commandWorked(coll.createIndex({[metaFieldName]: 1}));

    if (TimeseriesTest.timeseriesMetricIndexesEnabled(db.getMongo())) {
        assert.commandWorked(coll.createIndex({x: 1}, {name: "x"}));
    }

    const checkIndexSpec = function(spec, userIndex) {
        assert(spec.hasOwnProperty("v"));
        assert(spec.hasOwnProperty("name"));
        assert(spec.hasOwnProperty("key"));

        if (userIndex) {
            assert(!spec.hasOwnProperty("originalSpec"));

            if (spec.name == "x") {
                assert.eq(spec.key, {x: 1});
            }

            return;
        }

        if (spec.name == "x") {
            assert(spec.hasOwnProperty("originalSpec"));
            assert.eq(spec.v, spec.originalSpec.v);
            assert.eq(spec.name, spec.originalSpec.name);
            assert.neq(spec.key, spec.originalSpec.key);

            assert.eq(spec.key, {"control.min.x": 1, "control.max.x": 1});
        } else {
            assert(!spec.hasOwnProperty("originalSpec"));
        }
    };

    let userIndexes = coll.getIndexes();
    for (const index of userIndexes) {
        checkIndexSpec(index, /*userIndex=*/true);
    }

    let bucketIndexes = bucketsColl.getIndexes();
    for (const index of bucketIndexes) {
        checkIndexSpec(index, /*userIndex=*/false);
    }

    // Creating an index directly on the buckets collection is permitted. However, these types of
    // index creations will not have an "originalSpec" field and rely on the reverse mapping
    // mechanism.
    if (TimeseriesTest.timeseriesMetricIndexesEnabled(db.getMongo())) {
        assert.commandWorked(
            bucketsColl.createIndex({"control.min.y": 1, "control.max.y": 1}, {name: "y"}));

        let foundIndex = false;
        bucketIndexes = bucketsColl.getIndexes();
        for (const index of bucketIndexes) {
            if (index.name == "y") {
                foundIndex = true;
                assert(!index.hasOwnProperty("originalSpec"));
                break;
            }
        }
        assert(foundIndex);

        // Verify that the bucket index can map to a user index.
        foundIndex = false;
        userIndexes = coll.getIndexes();
        for (const index of userIndexes) {
            if (index.name == "y") {
                foundIndex = true;
                assert(!index.hasOwnProperty("originalSpec"));
                assert.eq(index.key, {y: 1});
                break;
            }
        }
        assert(foundIndex);
    }
});
}());
