/**
 * Tests $indexStats on a time-series collection.
 *
 * @tags: [
 *   # This test attempts to perform write operations and get index usage statistics using the
 *   # $indexStats stage. The former operation must be routed to the primary in a replica set,
 *   # whereas the latter may be routed to a secondary.
 *   assumes_read_preference_unchanged,
 *   # This test depends on certain writes ending up in the same bucket. Stepdowns and tenant
 *   # migrations may result in writes splitting between two primaries, and thus different buckets.
 *   does_not_support_stepdowns,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   # TODO (SERVER-104682): Remove this exclusion tag once the bug is fixed
 *   # Explain on $indexStats aggregation with rawData on mongos
 *   # does not contain queryShapeHash field.
 *   known_query_shape_computation_problem,
 * ]
 */
import {
    createRawTimeseriesIndex,
    getTimeseriesCollForRawOps,
    kRawOperationSpec,
} from "jstests/core/libs/raw_operation_utils.js";
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {isShardedTimeseries} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";

TimeseriesTest.run((insert) => {
    const timeFieldName = "tm";
    const metaFieldName = "mm";

    const doc = {_id: 0, [timeFieldName]: ISODate(), [metaFieldName]: {tag1: "a", tag2: "b"}};

    const coll = db[jsTestName()];
    coll.drop();

    assert.commandWorked(
        db.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}),
    );

    assert.commandWorked(insert(coll, doc), "failed to insert doc: " + tojson(doc));

    const indexKeys = {
        index0: {[metaFieldName + ".tag1"]: 1},
        index1: {[metaFieldName + ".tag2"]: -1, [timeFieldName]: -1},
        index2: {[metaFieldName + ".tag3"]: 1, [metaFieldName + ".tag4"]: 1},
    };

    // An index on {metaField, timeField} gets built by default on time-series collections.
    indexKeys["mm_1_tm_1"] = {[metaFieldName]: 1, [timeFieldName]: 1};

    // Create a few indexes on the time-series collections that $indexStats should return.
    for (const [indexName, indexKey] of Object.entries(indexKeys)) {
        assert.commandWorked(
            coll.createIndex(indexKey, {name: indexName}),
            "failed to create index: " + indexName + ": " + tojson(indexKey),
        );
    }
    if (isShardedTimeseries(coll)) {
        // Expect one additional index, created implicitly when the collection was implicitly
        // sharded.
        const shardIndexName = "tm_1";
        indexKeys[shardIndexName] = {[timeFieldName]: 1};
    }

    // Create a raw index directly over the bucket documents that would not be visible in the
    // time-series collection $indexStats results due to a failed conversion.
    assert.commandWorked(
        createRawTimeseriesIndex(coll, {not_metadata: 1}, "bucketindex"),
        "failed to create index: " + tojson({not_metadata: 1}),
    );

    // Check that $indexStats aggregation stage returns key patterns that are consistent with the
    // ones provided to the createIndexes commands.
    const indexStatsDocs = coll.aggregate([{$indexStats: {}}]).toArray();
    assert.eq(Object.keys(indexKeys).length, indexStatsDocs.length, tojson(indexStatsDocs));
    for (let i = 0; i < indexStatsDocs.length; ++i) {
        const stat = indexStatsDocs[i];
        assert(
            indexKeys.hasOwnProperty(stat.name),
            "$indexStats returned unknown index: " + stat.name + ": " + tojson(indexStatsDocs),
        );
        assert.docEq(
            stat.key,
            indexKeys[stat.name],
            "$indexStats returned unexpected top-level key for index: " + stat.name + ": " + tojson(indexStatsDocs),
        );
        assert.docEq(
            stat.spec.key,
            indexKeys[stat.name],
            "$indexStats returned unexpected nested key in spec for index: " +
                stat.name +
                ": " +
                tojson(indexStatsDocs),
        );
    }

    // Confirm that that $indexStats is indeed ignoring one index in schema translation by checking
    // the raw indexes over the bucket documents of the time-series collection.
    const bucketIndexStatsDocs = getTimeseriesCollForRawOps(coll)
        .aggregate([{$indexStats: {}}], kRawOperationSpec)
        .toArray();
    assert.eq(Object.keys(indexKeys).length + 1, bucketIndexStatsDocs.length, tojson(bucketIndexStatsDocs));

    // Check that $indexStats is not limited to being the only stage in an aggregation pipeline on a
    // time-series collection.
    const multiStageDocs = coll
        .aggregate([{$indexStats: {}}, {$group: {_id: 0, index_names: {$addToSet: "$name"}}}])
        .toArray();
    assert.eq(1, multiStageDocs.length, tojson(multiStageDocs));
    assert.sameMembers(Object.keys(indexKeys), multiStageDocs[0].index_names, tojson(multiStageDocs));
});
