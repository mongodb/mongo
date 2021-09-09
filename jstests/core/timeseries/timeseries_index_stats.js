/**
 * Tests $indexStats on a time-series collection.
 *
 * @tags: [
 *     # This test attempts to perform write operations and get index usage statistics using the
 *     # $indexStats stage. The former operation must be routed to the primary in a replica set,
 *     # whereas the latter may be routed to a secondary.
 *     assumes_read_preference_unchanged,
 *     # Cannot implicitly shard accessed collections because of following errmsg: A single
 *     # update/delete on a sharded collection must contain an exact match on _id or contain the
 *     # shard key.
 *     assumes_unsharded_collection,
 *     does_not_support_stepdowns,
 *     does_not_support_transactions,
 *     requires_fcv_49,
 *     requires_find_command,
 *     requires_getmore,
 *     requires_non_retryable_writes,
 * ]
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");

TimeseriesTest.run((insert) => {
    const timeFieldName = 'tm';
    const metaFieldName = 'mm';

    const doc = {_id: 0, [timeFieldName]: ISODate(), [metaFieldName]: {tag1: 'a', tag2: 'b'}};

    const coll = db.timeseries_index_stats;
    const bucketsColl = db.getCollection('system.buckets.' + coll.getName());
    coll.drop();  // implicitly drops bucketsColl.

    assert.commandWorked(db.createCollection(
        coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));
    assert.contains(bucketsColl.getName(), db.getCollectionNames());

    assert.commandWorked(insert(coll, doc), 'failed to insert doc: ' + tojson(doc));

    const indexKeys = {
        index0: {[metaFieldName + '.tag1']: 1},
        index1: {[metaFieldName + '.tag2']: -1, [timeFieldName]: -1},
        index2: {[metaFieldName + '.tag3']: 1, [metaFieldName + '.tag4']: 1},
    };

    // Create a few indexes on the time-series collections that $indexStats should return.
    for (const [indexName, indexKey] of Object.entries(indexKeys)) {
        assert.commandWorked(coll.createIndex(indexKey, {name: indexName}),
                             'failed to create index: ' + indexName + ': ' + tojson(indexKey));
    }

    // Create an index directly on the buckets collection that would not be visible in the
    // time-series collection $indexStats results due to a failed conversion.
    assert.commandWorked(bucketsColl.createIndex({not_metadata: 1}, 'bucketindex'),
                         'failed to create index: ' + tojson({not_metadata: 1}));

    // Check that $indexStats aggregation stage returns key patterns that are consistent with the
    // ones provided to the createIndexes commands.
    const indexStatsDocs = coll.aggregate([{$indexStats: {}}]).toArray();
    assert.eq(Object.keys(indexKeys).length, indexStatsDocs.length, tojson(indexStatsDocs));
    for (let i = 0; i < indexStatsDocs.length; ++i) {
        const stat = indexStatsDocs[i];
        assert(indexKeys.hasOwnProperty(stat.name),
               '$indexStats returned unknown index: ' + stat.name + ': ' + tojson(indexStatsDocs));
        assert.docEq(indexKeys[stat.name],
                     stat.key,
                     '$indexStats returned unexpected top-level key for index: ' + stat.name +
                         ': ' + tojson(indexStatsDocs));
        assert.docEq(indexKeys[stat.name],
                     stat.spec.key,
                     '$indexStats returned unexpected nested key in spec for index: ' + stat.name +
                         ': ' + tojson(indexStatsDocs));
    }

    // Confirm that that $indexStats is indeed ignoring one index in schema translation by checking
    // $indexStats on the buckets collection.
    const bucketIndexStatsDocs = bucketsColl.aggregate([{$indexStats: {}}]).toArray();
    assert.eq(Object.keys(indexKeys).length + 1,
              bucketIndexStatsDocs.length,
              tojson(bucketIndexStatsDocs));

    // Check that $indexStats is not limited to being the only stage in an aggregation pipeline on a
    // time-series collection.
    const multiStageDocs =
        coll.aggregate([{$indexStats: {}}, {$group: {_id: 0, index_names: {$addToSet: '$name'}}}])
            .toArray();
    assert.eq(1, multiStageDocs.length, tojson(multiStageDocs));
    assert.sameMembers(
        Object.keys(indexKeys), multiStageDocs[0].index_names, tojson(multiStageDocs));
});
})();
