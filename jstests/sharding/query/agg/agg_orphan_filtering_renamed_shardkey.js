// Tests whether orphan document are filtered correctly for pipelines involving equalities on
// renamed shard key in presence of shards and moving chunks
// @tags: [requires_sharding, requires_fcv_82]

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const docs = [
    {_id: 0, shardKey: 6, data: NumberInt(0)},
    {_id: 1, shardKey: 0, data: NumberInt(0)},
];

// renaming sharding key and applying an $or on predicates on the renamed key and another field.
const renameShardKeyWithOrOnRenamedField = [
    {$project: {renamedShardKey: "$shardKey"}},
    {$match: {$or: [{renamedShardKey: 6, data: {$ne: 1}}]}},
];

// renaming sharding key and applying an $and on predicates on the renamed key and another field.
const renameShardKeyWithAndOnRenamedField = [
    {$project: {renamedShardKey: "$shardKey"}},
    {$match: {$and: [{renamedShardKey: 6, data: {$ne: 1}}]}},
];

// only applying an $or on predicates on the renamed key and another field.
const orOnShardKey = [{$match: {$or: [{shardKey: 6, data: {$ne: 1}}]}}];

// renaming sharding key and applying only an equality predicate on the renamed key.
const renameShardKeyWithSimpleQueryOnRenamedField = [
    {$project: {renamedShardKey: "$shardKey"}},
    {$match: {renamedShardKey: 6}},
];

// applying only an equality predicate on the shard key.
const simpleQueryOnShardKey = [{$match: {shardKey: 6}}];

// grouping on the sharding key and applying an equality on the grouping key.
const groupOnShardKeyAndQueryOnGroupField = [{$group: {_id: "$shardKey"}}, {$match: {_id: 6}}];

const queries = [
    renameShardKeyWithOrOnRenamedField,
    renameShardKeyWithAndOnRenamedField,
    orOnShardKey,
    renameShardKeyWithSimpleQueryOnRenamedField,
    simpleQueryOnShardKey,
    groupOnShardKeyAndQueryOnGroupField,
];

function dbHasOrphan(st, coll) {
    let allShards =
        st.getDB("admin")
            .aggregate([{$shardedDataDistribution: {}}, {$match: {ns: coll.getFullName()}}])
            .next();
    let hasOrphan = false;
    for (const shard of allShards.shards) {
        hasOrphan = hasOrphan || shard.numOrphanedDocs;
    }
    return hasOrphan;
}

const st = new ShardingTest({shards: 2, mongos: 1});
const shardedDb = st.getDB("test");
assert(st.adminCommand({enablesharding: "test", primaryShard: st.shard0.shardName}));
const coll = shardedDb.coll;

for (const query of queries) {
    assert(coll.drop());
    assert.commandWorked(coll.insert(docs));
    assert.commandWorked(coll.createIndex({shardKey: 1}));

    const resultsBeforeSharding = coll.aggregate(query).toArray();

    assert(st.adminCommand({shardcollection: coll.getFullName(), key: {shardKey: 1}}));
    const resultsAfterSharding = coll.aggregate(query).toArray();

    // The results before and after sharding should be identical.
    assert.eq(
        resultsBeforeSharding, resultsAfterSharding, {resultsBeforeSharding, resultsAfterSharding});

    // one doc on each shard
    assert(st.adminCommand({split: coll.getFullName(), middle: {shardKey: 3}}));
    const resultsAfterSplitting = coll.aggregate(query).toArray();

    // The results before and after splitting should be identical.
    assert.eq(resultsBeforeSharding,
              resultsAfterSplitting,
              {resultsBeforeSharding, resultsAfterSplitting});

    let suspendRangeDeletionShard0Fp =
        configureFailPoint(st.rs0.getPrimary(), "suspendRangeDeletion");

    // shard0 will have shardKey=0 doc and the orphaned shardKey=6 doc, and shard1 will have only
    // the shardKey=6 document.
    assert(st.adminCommand(
        {moveChunk: coll.getFullName(), find: {shardKey: 6}, to: st.shard1.shardName}));
    suspendRangeDeletionShard0Fp.wait();

    const resultsAfterMovingChunk = coll.aggregate(query).toArray();

    // ensure there is an orphaned document before the query.
    assert.eq(dbHasOrphan(st, coll), 1);

    // Assert on results of queries despite orphaned documents.
    // The results before and after moving chunks should be identical.
    assert.eq(resultsBeforeSharding,
              resultsAfterMovingChunk,
              {resultsBeforeSharding, resultsAfterMovingChunk});

    // ensure there is an orphaned document even after the query.
    assert.eq(dbHasOrphan(st, coll), 1);

    // release range deletion to clean orphaned documents
    suspendRangeDeletionShard0Fp.off();
}

st.stop();
