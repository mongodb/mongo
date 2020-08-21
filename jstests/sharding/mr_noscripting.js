// Tests that running mapReduce does not crash anything if the shards have scripting disabled.
// @tags: [requires_fcv_44]
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

// Shard the collection and make sure there is a non-empty chunk on each shard.
st.shardColl(coll.getName(), {x: 1}, {x: 0}, {x: 1});
assert.commandWorked(coll.insert([{x: 1}, {x: -1}]));

const mapFn = function() {
    emit(this.x, 1);
};

const reduceFn = function(key, values) {
    return 1;
};

const mrCmd = {
    mapreduce: 'bar',
    map: mapFn,
    reduce: reduceFn,
    out: {inline: 1}
};
// If the command succeeds when it should have failed, having the explain plan in the log may help.
assert.commandFailedWithCode(testDB.runCommand(mrCmd), 31264, () => {
    const explain = tojson(testDB.runCommand({explain: mrCmd}));
    return `mapReduce command was expected to fail, but succeeded; explain plan is ${explain}`;
});

st.stop();
}());
