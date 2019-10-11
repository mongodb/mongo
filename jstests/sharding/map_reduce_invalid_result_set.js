// Tests that mapReduce commands fail if the result set does not fit into a single batch.
// @tags: [
//   uses_map_reduce_with_temp_collections,
//   does_not_support_stepdowns,
// ]
(function() {

"use strict";

// This test assumes we are running against the new version of mapReduce.
if (TestData.setParameters.internalQueryUseAggMapReduce != 1) {
    return;
}

const st = new ShardingTest({shards: 2});
const testDB = st.getDB("test");
const coll = "map_reduce_invalid_result_set";

assert.commandWorked(st.s.adminCommand({enablesharding: "test"}));
st.ensurePrimaryShard("test", st.shard0.shardName);

const lengthPerString = 1 * 1024 * 1024;
const nDocs = 17;
const bulk = testDB[coll].initializeUnorderedBulkOp();
for (let i = 0; i < nDocs; ++i) {
    bulk.insert({_id: i, key: i % 2, y: Array(lengthPerString).join("a")});
}
assert.commandWorked(bulk.execute());

function runLimitTests(dbConn, expectedError) {
    function mapFunc() {
        emit(this.key, this.y);
    }
    function reduceFunc(key, values) {
        return values.join('');
    }

    // Test that output mode inline correctly fails since the byte size of the results are greater
    // than 16MB.
    assert.commandFailedWithCode(
        dbConn.runCommand({mapReduce: coll, map: mapFunc, reduce: reduceFunc, out: {inline: 1}}),
        expectedError);

    // Verify that neither mongos nor the shards leave any cursors open.
    assert.eq(st.s.getDB("test").serverStatus().metrics.cursor.open.total, 0);
    assert.soon(() => st.shard0.getDB("test").serverStatus().metrics.cursor.open.total == 0);
    assert.soon(() => st.shard1.getDB("test").serverStatus().metrics.cursor.open.total == 0);

    // Test that non-inline output succeeds since no individual document is over the 16MB limit.
    assert.commandWorked(
        dbConn.runCommand({mapReduce: coll, map: mapFunc, reduce: reduceFunc, out: coll + "_out"}));
    assert.eq(2, dbConn[coll + "_out"].find().itcount());

    // Removing two documents puts the size of results just under 16MB, and thus inline should pass.
    assert.commandWorked(dbConn[coll].remove({_id: 0}));
    assert.commandWorked(dbConn[coll].remove({_id: 1}));
    assert.commandWorked(
        dbConn.runCommand({mapReduce: coll, map: mapFunc, reduce: reduceFunc, out: {inline: 1}}));

    // Re-insert the docs to keep the collection state consistent.
    assert.commandWorked(
        dbConn[coll].insert({_id: 0, key: 0, y: Array(lengthPerString).join("a")}));
    assert.commandWorked(
        dbConn[coll].insert({_id: 1, key: 1, y: Array(lengthPerString).join("a")}));
}

// First run directly against the primary shard. This is meant to test the mongod translation of
// mapReduce.
runLimitTests(st.rs0.getPrimary().getDB("test"), ErrorCodes.BSONObjectTooLarge);

// Next test against an unsharded collection but through mongos.
runLimitTests(testDB, 31301);

// Shard the input collection and re-run the tests.
st.shardColl(testDB[coll], {_id: 1}, {_id: "hashed"});
runLimitTests(testDB, 31301);

st.stop();
}());
