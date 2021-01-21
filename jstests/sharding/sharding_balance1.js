(function() {
'use strict';

load("jstests/sharding/libs/find_chunks_util.js");

var st = new ShardingTest({shards: 2, mongos: 1, other: {chunkSize: 1, enableBalancer: true}});

const dbName = 'ShardingBalanceTest';
const collName = 'foo';
const coll = st.getDB(dbName).getCollection(collName);
const minChunkNum = 20;

assert.commandWorked(st.s.adminCommand({enablesharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard1.shardName);

const bigStringSize = 10000;
const bigString = "X=".repeat(bigStringSize / 2);

jsTest.log("Inserting documents that will account for at least 20MB");
var insertedChars = 0;
var num = 0;
var bulk = coll.initializeUnorderedBulkOp();
while (insertedChars < (minChunkNum * 1024 * 1024)) {
    bulk.insert({_id: num++, s: bigString});
    insertedChars += bigStringSize;
}
assert.commandWorked(bulk.execute());

assert.commandWorked(st.s.adminCommand({shardcollection: coll.getFullName(), key: {_id: 1}}));
jsTest.log("Checking initial chunk distribution: " + st.chunkCounts(collName, dbName));
assert.lt(minChunkNum,
          findChunksUtil.countChunksForNs(st.config, coll.getFullName()),
          "Number of initial chunks is less then expected");
assert.lt(minChunkNum,
          st.chunkDiff(collName, dbName),
          "The initial chunks difference between the shards is less then expected");

jsTest.log("Await for the balancer to reduce the chunk imbalance between the shards");
// Make sure there's enough time here, since balancing can sleep for 15s or so between balances.
st.awaitBalance(collName, dbName, 1000 * 60 * 5 /* 5 min */);

st.stop();
})();
