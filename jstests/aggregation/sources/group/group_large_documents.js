/**
 * Test that $group can generate documents larger than maximum BSON size, as long as only part of
 * such document is returned to the client.
 * @tags: [
 *   # For sharded collections, we pushdown $group to shards to perform pre-aggregation and then
 *   # compute the final result on mongos. Since $group executed on shard produces documents larger
 *   # than maximum BSON size, we cannot serialize the result and send it to mongos. Such problem
 *   # does not exist in standalone and replica set setups.
 *   assumes_against_mongod_not_mongos,
 * ]
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For arrayEq.

const coll = db.group_large_documents_local;
coll.drop();

const largeString = 'x'.repeat(10 * 1024 * 1024);
for (let i = 0; i < 7; ++i) {
    assert.commandWorked(coll.insert({key: 1, largeField: largeString}));
}

for (let preventProjectPushdown of [false, true]) {
    const pipeline = [{$group: {_id: "$key", out: {$push: "$largeField"}}}];
    if (preventProjectPushdown) {
        pipeline.push({$_internalInhibitOptimization: {}});
    }
    pipeline.push({$project: {_id: 0, a: {$add: [1, "$_id"]}}});

    const results = coll.aggregate(pipeline).toArray();

    assert(arrayEq(results, [{a: 2}]),
           "Pipeline:\n" + tojson(pipeline) + "Actual results:\n" + tojson(results));
}
}());
