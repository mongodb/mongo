/**
 * Tests findAndModify on a sharded time-series buckets collection.
 *
 * @tags: [
 *   # TODO (SERVER-76583): Remove this tag.
 *   does_not_support_retryable_writes,
 *   featureFlagTimeseriesUpdatesSupport,
 *   # TODO (SERVER-76583): Remove this tag.
 *   requires_non_retryable_writes,
 *   requires_timeseries,
 * ]
 */

import {
    doc1_a_nofields,
    doc2_a_f101,
    doc3_a_f102,
    doc4_b_f103,
    doc5_b_f104,
    doc6_c_f105,
    doc7_c_f106,
    getCallerName,
    prepareShardedCollection,
    setUpShardedCluster,
    sysCollNamePrefix,
    tearDownShardedCluster,
} from "jstests/core/timeseries/libs/timeseries_writes_util.js";
import {withTxnAndAutoRetryOnMongos} from "jstests/libs/auto_retry_transaction_in_sharding.js";

const docs = [
    doc1_a_nofields,
    doc2_a_f101,
    doc3_a_f102,
    doc4_b_f103,
    doc5_b_f104,
    doc6_c_f105,
    doc7_c_f106,
];

Random.setRandomSeed();

setUpShardedCluster();

function prepareShardedBucketsCollection(initialDocList) {
    const coll =
        prepareShardedCollection({collName: getCallerName(), initialDocList: initialDocList});
    return coll.getDB().getCollection(sysCollNamePrefix + coll.getName());
}

const testBucketDelete = function(queryField) {
    const sysColl = prepareShardedBucketsCollection(docs);

    const orgBucketDocs = sysColl.find().toArray();
    const bucketDocIdx = Random.randInt(orgBucketDocs.length);
    const res = assert.commandWorked(sysColl.runCommand({
        findAndModify: sysColl.getName(),
        query: {[queryField]: orgBucketDocs[bucketDocIdx][queryField]},
        remove: true,
    }));
    assert.eq(1, res.lastErrorObject.n, `findAndModify failed: ${tojson(res)}`);

    const newBucketDocs = sysColl.find().toArray();
    assert.eq(orgBucketDocs.length - 1,
              newBucketDocs.length,
              `Wrong number of buckets left: ${tojson(newBucketDocs)}`);
    assert(!newBucketDocs.find(e => e === orgBucketDocs[bucketDocIdx]), tojson(newBucketDocs));
};
testBucketDelete("_id");
testBucketDelete("meta");

const testBucketMetaUpdate = function(queryField) {
    const sysColl = prepareShardedBucketsCollection(docs);

    const orgBucketDocs = sysColl.find().toArray();
    const bucketDocIdx = orgBucketDocs.findIndex(e => e.meta === "C");

    // Shard key change can be done inside a transaction.
    const session = sysColl.getDB().getMongo().startSession();
    const sessionDb = session.getDatabase(sysColl.getDB().getName());
    withTxnAndAutoRetryOnMongos(session, () => {
        // According to the default split point, this does not change the owning shard.
        const res = assert.commandWorked(sessionDb.runCommand({
            findAndModify: sysColl.getName(),
            query: {[queryField]: orgBucketDocs[bucketDocIdx][queryField]},
            update: {$set: {meta: "D"}},
            new: true,
        }));
        assert.eq(1, res.lastErrorObject.n, `findAndModify failed: ${tojson(res)}`);
        assert.eq("D", res.value.meta, `Wrong meta field: ${tojson(res)}`);
    });

    const newBucketDocs = sysColl.find().toArray();
    assert.eq(orgBucketDocs.length,
              newBucketDocs.length,
              `Wrong number of buckets left: ${tojson(newBucketDocs)}`);
    assert(!newBucketDocs.find(e => e === orgBucketDocs[bucketDocIdx]), tojson(newBucketDocs));
};
testBucketMetaUpdate("_id");
testBucketMetaUpdate("meta");

const testBucketMetaUpdateToOwningShardChange = function(queryField) {
    const sysColl = prepareShardedBucketsCollection(docs);
    const orgBucketDocs = sysColl.find().toArray();
    const bucketDocIdx = orgBucketDocs.findIndex(e => e.meta === "C");
    // Shard key change can be done inside a transaction.
    const session = sysColl.getDB().getMongo().startSession();
    const sessionDb = session.getDatabase(sysColl.getDB().getName());
    withTxnAndAutoRetryOnMongos(session, () => {
        // According to the default split point, this changes the owning shard.
        const res = assert.commandWorked(sessionDb.runCommand({
            findAndModify: sysColl.getName(),
            query: {[queryField]: orgBucketDocs[bucketDocIdx][queryField]},
            update: {$set: {meta: "A"}},
            new: true,
        }));
        assert.eq(1, res.lastErrorObject.n, `findAndModify failed: ${tojson(res)}`);
        assert.eq("A", res.value.meta, `Wrong meta field: ${tojson(res)}`);
    });
    const newBucketDocs = sysColl.find().toArray();
    assert.eq(orgBucketDocs.length,
              newBucketDocs.length,
              `Wrong number of buckets left: ${tojson(newBucketDocs)}`);
    assert(!newBucketDocs.find(e => e === orgBucketDocs[bucketDocIdx]), tojson(newBucketDocs));
};
testBucketMetaUpdateToOwningShardChange("_id");
testBucketMetaUpdateToOwningShardChange("meta");

tearDownShardedCluster();
