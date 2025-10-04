// This test is to ensure that limit() clauses are pushed down to the shards and evaluated
// See: http://jira.mongodb.org/browse/SERVER-1896
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";

let s = new ShardingTest({name: "limit_push", shards: 2, mongos: 1});
var db = s.getDB("test");
s.adminCommand({enablesharding: "test", primaryShard: s.shard1.shardName});

// Create some data
for (let i = 0; i < 100; i++) {
    db.limit_push.insert({_id: i, x: i});
}
db.limit_push.createIndex({x: 1});
assert.eq(100, db.limit_push.find().length(), "Incorrect number of documents");

// Shard the collection
s.adminCommand({shardcollection: "test.limit_push", key: {x: 1}});

// Now split the and move the data between the shards
s.adminCommand({split: "test.limit_push", middle: {x: 50}});
s.adminCommand({
    moveChunk: "test.limit_push",
    find: {x: 51},
    to: s.getOther(s.getPrimaryShard("test")).name,
    _waitForDelete: true,
});

// Check that the chunck have split correctly
assert.eq(2, findChunksUtil.countChunksForNs(s.config, "test.limit_push"), "wrong number of chunks");

// The query is asking for the maximum value below a given value
// db.limit_push.find( { x : { $lt : 60} } ).sort( { x:-1} ).limit(1)
let q = {x: {$lt: 60}};

// Make sure the basic queries are correct
assert.eq(60, db.limit_push.find(q).count(), "Did not find 60 documents");
// rs = db.limit_push.find( q ).sort( { x:-1} ).limit(1)
// assert.eq( rs , { _id : "1" , x : 59 } , "Did not find document with value 59" );

// Now make sure that the explain shos that each shard is returning a single document as
// indicated
// by the "n" element for each shard
let exp = db.limit_push.find(q).sort({x: -1}).limit(1).explain("executionStats");
printjson(exp);

let execStages = exp.executionStats.executionStages;
assert.eq("SHARD_MERGE_SORT", execStages.stage, "Expected SHARD_MERGE_SORT as root stage");

let k = 0;
for (let j in execStages.shards) {
    assert.eq(1, execStages.shards[j].executionStages.nReturned, "'n' is not 1 from shard000" + k.toString());
    k++;
}

s.stop();
