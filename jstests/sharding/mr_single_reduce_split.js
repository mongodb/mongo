/**
 * This jstest verifies that the mrEnableSingleReduceOptimization flag works properly in a sharded
 * cluster when there are documents on multiple chunks that need to be merged.
 * @tags: [
 *  backport_required_multiversion,
 * ]
 */
(function() {
const st = new ShardingTest({
    shards: 2,
    mongos: 1,
    other: {
        mongosOptions: {setParameter: {mrEnableSingleReduceOptimization: true}},
        shardOptions: {setParameter: {mrEnableSingleReduceOptimization: true}},
    }
});

const mongosDB = st.s0.getDB(jsTestName());
const mongosColl = mongosDB[jsTestName()];

assert.commandWorked(mongosDB.dropDatabase());
assert.commandWorked(mongosDB.adminCommand({enableSharding: mongosDB.getName()}));
st.ensurePrimaryShard(mongosDB.getName(), st.shard0.shardName);

assert.commandWorked(
    mongosDB.adminCommand({shardCollection: mongosColl.getFullName(), key: {_id: 1}}));

// Split the collection into 2 chunks: [minKey, 0) and [0, maxKey].
assert.commandWorked(mongosDB.adminCommand({split: mongosColl.getFullName(), middle: {_id: 0}}));

// Move the [0, MaxKey) chunk to the second shard.
assert.commandWorked(mongosDB.adminCommand(
    {moveChunk: mongosColl.getFullName(), find: {_id: 50}, to: st.shard1.shardName}));

assert.commandWorked(mongosColl.insert({_id: -1}));
const map = function() {
    emit(0, {val: "mapped value"});
};

const reduce = function(key, values) {
    return {val: "reduced value"};
};

let res = assert.commandWorked(mongosDB.runCommand(
    {mapReduce: mongosColl.getName(), map: map, reduce: reduce, out: {inline: 1}}));
assert.eq(res.results[0], {_id: 0, value: {val: "mapped value"}});

assert.commandWorked(mongosColl.insert({_id: 1}));

res = assert.commandWorked(mongosDB.runCommand(
    {mapReduce: mongosColl.getName(), map: map, reduce: reduce, out: {inline: 1}}));
assert.eq(res.results[0], {_id: 0, value: {val: "reduced value"}});

st.stop();
}());
