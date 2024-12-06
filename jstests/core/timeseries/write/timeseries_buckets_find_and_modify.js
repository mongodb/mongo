/**
 * Tests findAndModify on a timeseries buckets collection.
 *
 * @tags: [
 *   # We need a timeseries collection.
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
    prepareCollection,
    sysCollNamePrefix,
} from "jstests/core/timeseries/libs/timeseries_writes_util.js";

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

function prepareBucketsCollection(initialDocList) {
    const coll = prepareCollection({collName: getCallerName(), initialDocList: initialDocList});
    return coll.getDB().getCollection(sysCollNamePrefix + coll.getName());
}

(function testBucketDeleteById() {
    const sysColl = prepareBucketsCollection(docs);

    const orgBucketDocs = sysColl.find().toArray();
    const bucketDocIdx = Random.randInt(orgBucketDocs.length);
    const res = assert.commandWorked(sysColl.runCommand({
        findAndModify: sysColl.getName(),
        query: {_id: orgBucketDocs[bucketDocIdx]._id},
        remove: true
    }));
    assert.eq(1, res.lastErrorObject.n, `findAndModify failed: ${tojson(res)}`);

    const newBucketDocs = sysColl.find().toArray();
    assert.eq(orgBucketDocs.length - 1,
              newBucketDocs.length,
              `Wrong number of buckets left: ${tojson(newBucketDocs)}`);
    assert(!newBucketDocs.find(e => e === orgBucketDocs[bucketDocIdx]), tojson(newBucketDocs));
})();

(function testBucketMetaUpdateById() {
    const sysColl = prepareBucketsCollection(docs);

    const orgBucketDocs = sysColl.find().toArray();
    const bucketDocIdx = Random.randInt(orgBucketDocs.length);
    const res = assert.commandWorked(sysColl.runCommand({
        findAndModify: sysColl.getName(),
        query: {_id: orgBucketDocs[bucketDocIdx]._id},
        update: {$set: {meta: "D"}},
        new: true
    }));
    assert.eq(1, res.lastErrorObject.n, `findAndModify failed: ${tojson(res)}`);
    assert.eq("D", res.value.meta, `Wrong meta field: ${tojson(res)}`);

    const newBucketDocs = sysColl.find().toArray();
    assert.eq(orgBucketDocs.length,
              newBucketDocs.length,
              `Wrong number of buckets left: ${tojson(newBucketDocs)}`);
    assert(!newBucketDocs.find(e => e === orgBucketDocs[bucketDocIdx]), tojson(newBucketDocs));
})();
