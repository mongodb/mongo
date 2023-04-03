/**
 * Tests that $percentile is computed correctly for sharded collections.
 * @tags: [
 *   requires_fcv_70,
 *   featureFlagApproxPercentiles
 * ]
 */

(function() {
const st = new ShardingTest({shards: 2, mongos: 1});

const db = st.s0.getDB(jsTestName());
const coll = db[jsTestName()];
const collUnsharded = db[jsTestName() + "_unsharded"];

assert.commandWorked(db.dropDatabase());

// Enable sharding on the test DB and ensure its primary is shard0.
assert.commandWorked(db.adminCommand({enableSharding: db.getName()}));
st.ensurePrimaryShard(db.getName(), st.shard0.shardName);

// Range-shard the test collection on _id.
assert.commandWorked(db.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));

// Split the collection into 4 chunks: [MinKey, -100), [-100, 0), [0, 100), [100, MaxKey).
assert.commandWorked(db.adminCommand({split: coll.getFullName(), middle: {_id: -100}}));
assert.commandWorked(db.adminCommand({split: coll.getFullName(), middle: {_id: 0}}));
assert.commandWorked(db.adminCommand({split: coll.getFullName(), middle: {_id: 100}}));

// Move the [0, 100) and [100, MaxKey) chunks to shard1.
assert.commandWorked(
    db.adminCommand({moveChunk: coll.getFullName(), find: {_id: 50}, to: st.shard1.shardName}));
assert.commandWorked(
    db.adminCommand({moveChunk: coll.getFullName(), find: {_id: 150}, to: st.shard1.shardName}));

function runTest({testName}) {
    const p = 0.9;
    const percentileSpec = {$percentile: {p: [p], input: "$x", method: "approximate"}};

    const res = coll.aggregate([{$group: {_id: null, p: percentileSpec}}]).toArray();
    const resUnsharded =
        collUnsharded.aggregate([{$group: {_id: null, p: percentileSpec}}]).toArray();

    // Compute the expected value of the percentile without using $percentile.
    const expected =
        collUnsharded
            .aggregate(
                [{$match: {x: {$type: "number"}}}, {$group: {_id: null, data: {$push: "$x"}}}])
            .toArray();

    let expectedResult = [null];
    if (expected[0]) {
        const data = expected[0].data;
        data.sort(function(a, b) {
            return a - b;
        });  // sort ASC
        expectedResult = data[Math.ceil(p * data.length) - 1];
    }

    // Check that unsharded is producing the expected result, so that we can differentiate between
    // failures in sharding vs general regressions in $percentile.
    assert.eq(expectedResult, resUnsharded[0].p, testName + ` result: ${tojson(resUnsharded)}`);

    // For approximate percentiles, the result might differ slightly as it depends on the order in
    // which the data is processed, but on small datasets we expect it to be accurate.
    assert.eq(expectedResult, res[0].p, testName + ` result: ${tojson(res)}`);
}

(function testMergingAcrossAllShards() {
    coll.remove({});
    collUnsharded.remove({});

    // Write 400 documents across the 4 chunks. Values of 'val' field are distributed across both
    // shards such that their ranges overlap but the datasets aren't identical
    for (let i = -200; i < 200; i++) {
        const val = (i + 200) % 270 + 1;  // [1, ..., 200] & [201, ..., 270, 1, ..., 130]
        assert.commandWorked(coll.insert({_id: i, x: val}));
        assert.commandWorked(collUnsharded.insert({_id: i, x: val}));
    }

    runTest("testMergingAcrossAllShards");
})();

(function testMergingOneShardNonNumeric() {
    coll.remove({});
    collUnsharded.remove({});

    for (let i = -200; i < 0; i++) {
        // shard0 -- numeric values of "x"
        assert.commandWorked(coll.insert({_id: i, x: i}));
        assert.commandWorked(collUnsharded.insert({_id: i, x: i}));

        // shard1: non-numeric values of "x"
        assert.commandWorked(coll.insert({_id: -i, x: "val" + i}));
        assert.commandWorked(collUnsharded.insert({_id: -i, x: "val" + i}));
    }

    runTest("testMergingOneShardNonNumeric");
})();

(function testMergingAllDataNonNumeric() {
    coll.remove({});
    collUnsharded.remove({});

    for (let i = -200; i < 200; i++) {
        assert.commandWorked(coll.insert({_id: i, x: "val" + i}));
        assert.commandWorked(collUnsharded.insert({_id: i, x: "val" + i}));
    }

    runTest("testMergingOneShardNonNumeric");
})();

st.stop();
})();
