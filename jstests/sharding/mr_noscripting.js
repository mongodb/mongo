// Tests that running mapReduce does not crash anything if the shards have scripting disabled.
(function() {
"use strict";
const shardOpts = [
    {noscripting: ''},
    {}  // just use default params
];

const st = new ShardingTest({shards: shardOpts});
const mongos = st.s;

const testDB = mongos.getDB('test');
const coll = testDB.bar;

// Shard the collection and make sure there is a chunk on each shard.
st.shardColl(coll.getName(), {x: 1}, {x: 0}, {x: 1});

assert.commandWorked(coll.insert({x: 1}));

const mapFn = function() {
    emit(this.x, 1);
};

const reduceFn = function(key, values) {
    return 1;
};

assert.commandFailedWithCode(
    testDB.runCommand({mapreduce: 'bar', map: mapFn, reduce: reduceFn, out: {inline: 1}}), 16149);

st.stop();
}());
