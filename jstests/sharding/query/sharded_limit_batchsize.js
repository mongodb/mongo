// Tests for sharded limit + batchSize. Make sure that various combinations
// of limit and batchSize with sort return the correct results, and do not issue
// unnecessary getmores (see SERVER-14299).
import {ShardingTest} from "jstests/libs/shardingtest.js";

/**
 * Test the correctness of queries with sort and batchSize on a sharded cluster,
 * running the queries against collection 'coll'.
 */
function testBatchSize(coll) {
    // Roll the cursor over the second batch and make sure it's correctly sized
    assert.eq(20, coll.find().sort({x: 1}).batchSize(3).itcount());
    assert.eq(15, coll.find().sort({x: 1}).batchSize(3).skip(5).itcount());
}

/**
 * Test the correctness of queries with sort and limit on a sharded cluster,
 * running the queries against collection 'coll'.
 */
function testLimit(coll) {
    let cursor = coll.find().sort({x: 1}).limit(3);
    assert.eq(-10, cursor.next()["_id"]);
    assert.eq(-9, cursor.next()["_id"]);
    assert.eq(-8, cursor.next()["_id"]);
    assert(!cursor.hasNext());

    assert.eq(13, coll.find().sort({x: 1}).limit(13).itcount());

    cursor = coll.find().sort({x: 1}).skip(5).limit(2);
    assert.eq(-5, cursor.next()["_id"]);
    assert.eq(-4, cursor.next()["_id"]);
    assert(!cursor.hasNext());

    cursor = coll.find().sort({x: 1}).skip(9).limit(2);
    assert.eq(-1, cursor.next()["_id"]);
    assert.eq(1, cursor.next()["_id"]);
    assert(!cursor.hasNext());

    cursor = coll.find().sort({x: 1}).skip(11).limit(2);
    assert.eq(2, cursor.next()["_id"]);
    assert.eq(3, cursor.next()["_id"]);
    assert(!cursor.hasNext());

    // Ensure that in the limit 1 case the server does not leave a cursor open.
    let openCursorsBefore = assert.commandWorked(coll.getDB().serverStatus()).metrics.cursor.open.total;
    cursor = coll.find().sort({x: 1}).limit(1);
    assert(cursor.hasNext());
    assert.eq(-10, cursor.next()["_id"]);
    let openCursorsAfter = assert.commandWorked(coll.getDB().serverStatus()).metrics.cursor.open.total;
    assert.eq(openCursorsBefore, openCursorsAfter);
}

/**
 * Test correctness of queries run with singleBatch=true.
 */
function testSingleBatch(coll, numShards) {
    // Ensure that singleBatch queries that require multiple batches from individual shards
    // return complete results.
    let batchSize = 5;
    let res = assert.commandWorked(
        coll.getDB().runCommand({
            find: coll.getName(),
            filter: {x: {$lte: 10}},
            skip: numShards * batchSize,
            singleBatch: true,
            batchSize: batchSize,
        }),
    );
    assert.eq(batchSize, res.cursor.firstBatch.length);
    assert.eq(0, res.cursor.id);
    let cursor = coll
        .find()
        .skip(numShards * batchSize)
        .limit(-1 * batchSize);
    assert.eq(batchSize, cursor.itcount());
    cursor = coll
        .find()
        .skip(numShards * batchSize)
        .batchSize(-1 * batchSize);
    assert.eq(batchSize, cursor.itcount());
}

//
// Create a two-shard cluster. Have an unsharded collection and a sharded collection.
//

let st = new ShardingTest({shards: 2, other: {rsOptions: {setParameter: {"enableTestCommands": 1}}}});

var db = st.s.getDB("test");
let shardedCol = db.getCollection("sharded_limit_batchsize");
let unshardedCol = db.getCollection("unsharded_limit_batchsize");
shardedCol.drop();
unshardedCol.drop();

// Enable sharding and pre-split the sharded collection.
assert.commandWorked(db.adminCommand({enableSharding: db.getName(), primaryShard: st.shard0.shardName}));
db.adminCommand({shardCollection: shardedCol.getFullName(), key: {_id: 1}});
assert.commandWorked(db.adminCommand({split: shardedCol.getFullName(), middle: {_id: 0}}));
assert.commandWorked(db.adminCommand({moveChunk: shardedCol.getFullName(), find: {_id: 0}, to: st.shard1.shardName}));

// Write 10 documents to shard 0, and 10 documents to shard 1 inside the sharded collection.
// Write 20 documents which all go to the primary shard in the unsharded collection.
for (let i = 1; i <= 10; ++i) {
    // These go to shard 1.
    assert.commandWorked(shardedCol.insert({_id: i, x: i}));

    // These go to shard 0.
    assert.commandWorked(shardedCol.insert({_id: -i, x: -i}));

    // These go to shard 0 inside the non-sharded collection.
    assert.commandWorked(unshardedCol.insert({_id: i, x: i}));
    assert.commandWorked(unshardedCol.insert({_id: -i, x: -i}));
}

//
// Run tests for singleBatch queries.
//

testSingleBatch(shardedCol, 2);
testSingleBatch(unshardedCol, 1);

//
// Run tests for batch size. These should issue getmores.
//

jsTest.log("Running batchSize tests against sharded collection.");
st.shard0.adminCommand({setParameter: 1, logLevel: 1});
testBatchSize(shardedCol);
st.shard0.adminCommand({setParameter: 1, logLevel: 0});

jsTest.log("Running batchSize tests against non-sharded collection.");
testBatchSize(unshardedCol);

//
// Run tests for limit. These should *not* issue getmores. We confirm this
// by enabling the getmore failpoint on the shards.
//

assert.commandWorked(
    st.shard0.getDB("test").adminCommand({configureFailPoint: "failReceivedGetmore", mode: "alwaysOn"}),
);

assert.commandWorked(
    st.shard1.getDB("test").adminCommand({configureFailPoint: "failReceivedGetmore", mode: "alwaysOn"}),
);

jsTest.log("Running limit tests against sharded collection.");
testLimit(shardedCol, st.shard0);

jsTest.log("Running limit tests against non-sharded collection.");
testLimit(unshardedCol, st.shard0);

st.stop();
