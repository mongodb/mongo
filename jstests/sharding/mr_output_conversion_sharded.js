(function() {
"use strict";

load("jstests/noPassthrough/libs/server_parameter_helpers.js");  // For setParameterOnAllHosts.
load("jstests/libs/discover_topology.js");                       // For findDataBearingNodes.

const assertCounts = countsObj => {
    assert.eq(Object.keys(countsObj).length, 4);
    assert.eq(countsObj.input, 0);
    assert.eq(countsObj.emit, 0);
    assert.eq(countsObj.reduce, 0);
    assert.eq(countsObj.output, 0);
};

const assertTiming = timingObj => {
    assert.eq(Object.keys(timingObj).length, 2);
    assert(isNumber(timingObj.shardProcessing));
    assert(isNumber(timingObj.postProcessing));
};

const assertShardCounts = shardsObj => {
    assert.gte(Object.keys(shardsObj).length, 1);
    for (const shardName in shardsObj) {
        const shard = shardsObj[shardName];
        assert.eq(shard.input, 0);
        assert.eq(shard.emit, 0);
        assert.eq(shard.reduce, 0);
        assert.eq(shard.output, 0);
    }
};

const assertPostProcessCounts = shardsObj => {
    assert.eq(Object.keys(shardsObj).length, 1);
    for (const shardName in shardsObj) {
        const shard = shardsObj[shardName];
        assert.eq(shard.input, 0);
        assert.eq(shard.reduce, 0);
        assert.eq(shard.output, 0);
    }
};

const assertInlineOutputFormat = res => {
    assert.commandWorked(res);
    assert(Array.isArray(res.results));
    assert(!res.result);
    assert(isNumber(res.timeMillis));
    assertCounts(res.counts);
    assertShardCounts(res.shardCounts);
    assertPostProcessCounts(res.postProcessCounts);
    assert(!res.timing);
};

const assertCollectionOutputFormat = (res, outColl, outDb = null) => {
    res = coll.runCommand(cmd);
    assert.commandWorked(res);
    assert(!res.results);
    if (outDb) {
        assert.eq(res.result.db, outDb);
        assert.eq(res.result.collection, outColl);
    } else {
        assert.eq(res.result, outColl);
    }
    assert(isNumber(res.timeMillis));
    assertCounts(res.counts);
    assertShardCounts(res.shardCounts);
    assertPostProcessCounts(res.postProcessCounts);
    assert(!res.timing);
};

const assertVerboseFormat = res => {
    assert.commandWorked(res);
    assert(Array.isArray(res.results));
    assert(!res.result);
    assert(isNumber(res.timeMillis));
    assertCounts(res.counts);
    assertShardCounts(res.shardCounts);
    assertPostProcessCounts(res.postProcessCounts);
    assert(res.timing);
    assertTiming(res.timing);
};

const st = new ShardingTest({
    shards: 2,
    verbose: 1,
    mongos: 1,
    other: {
        chunkSize: 1,
        enableBalancer: true,
    }
});

const nodeList = DiscoverTopology.findNonConfigNodes(st.s);
setParameterOnAllHosts(nodeList, "internalQueryUseAggMapReduce", true);

st.adminCommand({enablesharding: "mrShard"});
st.ensurePrimaryShard('mrShard', st.shard1.shardName);
st.adminCommand({shardcollection: "mrShard.srcSharded", key: {"_id": 1}});

const coll = st.getDB("mrShard").mr_output_conversion_sharded;

const bulk = coll.initializeUnorderedBulkOp();
for (let j = 0; j < 100; j++) {
    for (let i = 1; i <= 512; ++i) {
        if (i % 3 === 0) {
            bulk.insert({user: `user_${i}`, colour: 'red'});
        } else if (i % 3 === 1) {
            bulk.insert({user: `user_${i}`, colour: 'blue'});
        } else {
            bulk.insert({user: `user_${i}`});
        }
    }
}
assert.commandWorked(bulk.execute());
st.awaitBalance('srcSharded', 'mrShard', 5 * 60 * 1000);
st.stopBalancer();

function map() {
    emit(this.colour, 1);
}

function reduce(key, values) {
    return Array.sum(values);
}

// Inline should produce results array.
let cmd = {
    mapReduce: coll.getName(),
    map,
    reduce,
    out: {inline: 1},
};
let res = coll.runCommand(cmd);
assertInlineOutputFormat(res);

// Specifying output collection should produce {result: <collName>}
cmd = {
    mapReduce: coll.getName(),
    map,
    reduce,
    out: 'srcSharded'
};
assertCollectionOutputFormat(res, 'srcSharded');

// Specifying output collection and db should produce
// {result:{ db: <dbName>, collection:  <collName> }}
cmd = {
    mapReduce: coll.getName(),
    map,
    reduce,
    out: {replace: coll.getName(), db: coll.getDB().getName()}
};
assertCollectionOutputFormat(res, coll.getName(), coll.getDB().getName());

// Timing information should be present with verbose option.
cmd = {
    mapReduce: coll.getName(),
    map,
    reduce,
    out: {inline: 1},
    verbose: true
};
res = coll.runCommand(cmd);
assertVerboseFormat(res);
st.stop();
})();
