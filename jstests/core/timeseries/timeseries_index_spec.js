/**
 * Tests that the original user index definition is stored on the transformed index definition on
 * the buckets collection for newly supported index types introduced in v6.0. Indexes created
 * directly on the buckets collection do not have an original user index definition and rely on the
 * reverse mapping mechanism.
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

TimeseriesTest.run(() => {
    const collName = "timeseries_index_spec";

    const timeFieldName = "tm";
    const metaFieldName = "mm";

    const coll = db.getCollection(collName);
    const bucketsColl = db.getCollection("system.buckets." + collName);
    coll.drop();

    assert.commandWorked(db.createCollection(
        coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));

    const checkIndexSpec = function(spec, userIndex, shouldHaveOriginalSpec) {
        assert(spec.hasOwnProperty("v"));
        assert(spec.hasOwnProperty("name"));
        assert(spec.hasOwnProperty("key"));

        if (userIndex) {
            assert(!spec.hasOwnProperty("originalSpec"));
            return;
        }

        if (shouldHaveOriginalSpec) {
            assert(spec.hasOwnProperty("originalSpec"));
            assert.eq(spec.v, spec.originalSpec.v);
            assert.eq(spec.name, spec.originalSpec.name);
            assert.neq(spec.key, spec.originalSpec.key);
        } else {
            assert(!spec.hasOwnProperty("originalSpec"));
        }
    };

    const verifyAndDropIndex = function(shouldHaveOriginalSpec, indexName) {
        let sawIndex = false;

        let userIndexes = coll.getIndexes();
        for (const index of userIndexes) {
            if (index.name === indexName) {
                sawIndex = true;
                checkIndexSpec(index, /*userIndex=*/true, shouldHaveOriginalSpec);
            }
        }

        let bucketIndexes = bucketsColl.getIndexes();
        for (const index of bucketIndexes) {
            if (index.name === indexName) {
                sawIndex = true;
                checkIndexSpec(index, /*userIndex=*/false, shouldHaveOriginalSpec);
            }
        }

        assert(sawIndex,
               `Index with name: ${indexName} is missing: ${tojson({userIndexes, bucketIndexes})}`);

        assert.commandWorked(coll.dropIndexes(indexName));
    };

    assert.commandWorked(coll.createIndex({[timeFieldName]: 1}, {name: "timefield_downgradable"}));
    verifyAndDropIndex(/*shouldHaveOriginalSpec=*/false, "timefield_downgradable");

    assert.commandWorked(coll.createIndex({[metaFieldName]: 1}, {name: "metafield_downgradable"}));
    verifyAndDropIndex(/*shouldHaveOriginalSpec=*/false, "metafield_downgradable");

    assert.commandWorked(coll.createIndex({[timeFieldName]: 1, [metaFieldName]: 1},
                                          {name: "time_meta_field_downgradable"}));
    verifyAndDropIndex(/*shouldHaveOriginalSpec=*/false, "time_meta_field_downgradable");

    if (FeatureFlagUtil.isEnabled(db, "TimeseriesMetricIndexes")) {
        assert.commandWorked(coll.createIndex({x: 1}, {name: "x_1"}));
        verifyAndDropIndex(/*shouldHaveOriginalSpec=*/true, "x_1");

        assert.commandWorked(
            coll.createIndex({x: 1}, {name: "x_partial", partialFilterExpression: {x: {$gt: 5}}}));
        verifyAndDropIndex(/*shouldHaveOriginalSpec=*/true, "x_partial");

        assert.commandWorked(coll.createIndex(
            {[timeFieldName]: 1}, {name: "time_partial", partialFilterExpression: {x: {$gt: 5}}}));
        verifyAndDropIndex(/*shouldHaveOriginalSpec=*/true, "time_partial");

        assert.commandWorked(coll.createIndex(
            {[metaFieldName]: 1}, {name: "meta_partial", partialFilterExpression: {x: {$gt: 5}}}));
        verifyAndDropIndex(/*shouldHaveOriginalSpec=*/true, "meta_partial");

        assert.commandWorked(
            coll.createIndex({[metaFieldName]: 1, x: 1},
                             {name: "meta_x_partial", partialFilterExpression: {x: {$gt: 5}}}));
        verifyAndDropIndex(/*shouldHaveOriginalSpec=*/true, "meta_x_partial");
    }

    // Creating an index directly on the buckets collection is permitted. However, these types of
    // index creations will not have an "originalSpec" field and rely on the reverse mapping
    // mechanism.
    if (FeatureFlagUtil.isEnabled(db, "TimeseriesMetricIndexes")) {
        assert.commandWorked(
            bucketsColl.createIndex({"control.min.y": 1, "control.max.y": 1}, {name: "y"}));

        let foundIndex = false;
        let bucketIndexes = bucketsColl.getIndexes();
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
        let userIndexes = coll.getIndexes();
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
