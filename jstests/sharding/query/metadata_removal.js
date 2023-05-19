// Ensure that metadata fields (and only metadata) are removed before documents are returned to
// customers. As we can't insert documents with metadata, this test focuses on queries that might
// inject metadata when communication results to mongos for merging. In addition, we want to ensure
// that other fields beginning with a '$' are not modified before returning them to customers.
// @tags: [
//   requires_fcv_62
// ]

(function() {
"use strict";

function runTest(coll, keyword) {
    const document = ({_id: 5, x: 1, $set: {$inc: {x: 5}}});
    assert.commandWorked(coll.insert(document));

    // Leaves $set intact without shard merging.
    assert.eq([document], coll.find().hint({$natural: 1}).toArray());
    assert.eq([document], coll.find().hint({_id: 1}).toArray());

    // Leaves $set intact when shard merging on agg query.
    const docWithY = ({_id: 5, x: 1, $set: {$inc: {x: 5}}, y: 1});
    assert.eq([docWithY], coll.aggregate([{$addFields: {y: 1}}]).toArray());

    // Leaves $set intact when shard merging on sort query.
    assert.eq([document], coll.find().sort({_id: 1}).toArray());

    // Leaves added fields intact when user adds $ prefixed field in an agg pipeline.
    const docWithDollarY = ({_id: 5, x: 1, $set: {$inc: {x: 5}}, $y: 1});
    assert.eq(
        [docWithDollarY],
        coll.aggregate(
                [{$replaceWith: {$setField: {field: {$literal: "$y"}, input: "$$ROOT", value: 1}}}])
            .toArray());
}

const st = new ShardingTest({shards: 2});
try {
    assert.commandWorked(st.s0.adminCommand({enableSharding: 'test'}));
    st.ensurePrimaryShard('test', st.shard1.shardName);
    assert.commandWorked(st.s0.adminCommand({shardCollection: 'test.coll', key: {x: 'hashed'}}));

    runTest(st.getDB("test").coll);
} finally {
    st.stop();
}
})();
