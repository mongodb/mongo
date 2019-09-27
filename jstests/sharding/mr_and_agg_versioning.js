// Test that map reduce and aggregate properly handle shard versioning.
(function() {
"use strict";

const st = new ShardingTest({shards: 2, mongos: 3});

const dbName = jsTest.name();
const nsString = dbName + ".coll";
const numDocs = 50000;
const numKeys = 1000;

st.s.adminCommand({enableSharding: dbName});
st.ensurePrimaryShard(dbName, st.shard0.shardName);
st.s.adminCommand({shardCollection: nsString, key: {key: 1}});

// Load chunk data to the stale mongoses before moving a chunk
const staleMongos1 = st.s1;
const staleMongos2 = st.s2;
staleMongos1.getCollection(nsString).find().itcount();
staleMongos2.getCollection(nsString).find().itcount();

st.s.adminCommand({split: nsString, middle: {key: numKeys / 2}});
st.s.adminCommand({moveChunk: nsString, find: {key: 0}, to: st.shard1.shardName});

const bulk = st.s.getCollection(nsString).initializeUnorderedBulkOp();
for (let i = 0; i < numDocs; i++) {
    bulk.insert({_id: i, key: (i % numKeys), value: i % numKeys});
}
assert.commandWorked(bulk.execute());

// Add orphaned documents directly to the shards to ensure they are properly filtered out.
st.shard0.getCollection(nsString).insert({_id: 0, key: 0, value: 0});
st.shard1.getCollection(nsString).insert({_id: numDocs, key: numKeys, value: numKeys});

const map = function() {
    emit(this.key, this.value);
};
const reduce = function(k, values) {
    let total = 0;
    for (let i = 0; i < values.length; i++) {
        total += values[i];
    }
    return total;
};
function validateOutput(output) {
    assert.eq(output.length, numKeys, tojson(output));
    for (let i = 0; i < output.length; i++) {
        assert.eq(output[i]._id * (numDocs / numKeys), output[i].value, tojson(output));
    }
}

let res = staleMongos1.getCollection(nsString).mapReduce(map, reduce, {out: {inline: 1}});
validateOutput(res.results);

res = staleMongos2.getCollection(nsString).aggregate(
    [{$group: {_id: "$key", value: {$sum: "$value"}}}, {$sort: {_id: 1}}]);
validateOutput(res.toArray());

st.stop();
})();
