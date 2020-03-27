// Tests that the mapReduce command works correctly under all combinations of the input and output
// collections being sharded or unsharded.
// Map Reduce before 4.4. does not support outputting to a sharded collection whose shard key is
// {_id: "hashed"}.
// @tags: [requires_fcv_44]
(function() {
"use strict";

const st = new ShardingTest({shards: 2, other: {chunkSize: 1}});

const testDB = st.getDB("mrShard");
const inputColl = testDB.srcSharded;

st.adminCommand({enableSharding: testDB.getName()});
st.adminCommand({enableSharding: "mrShardOtherDB"});
st.ensurePrimaryShard(testDB.getName(), st.shard1.shardName);

const nDistinctKeys = 512;
const nValuesPerKey = 100;

function seedCollection() {
    const bulk = inputColl.initializeUnorderedBulkOp();
    for (let key = 0; key < nDistinctKeys; key++) {
        for (let value = 0; value < nValuesPerKey; value++) {
            bulk.insert({key: key, value: value});
        }
    }
    assert.commandWorked(bulk.execute());
}

function mapFn() {
    emit(this.key, 1);
}
function reduceFn(key, values) {
    return Array.sum(values);
}

function testMrOutput({inputSharded, outputSharded}) {
    inputColl.drop();
    if (inputSharded) {
        st.adminCommand({shardCollection: inputColl.getFullName(), key: {_id: "hashed"}});
    }
    seedCollection();
    const outputColl = testDB[inputColl.getName() + "Out"];
    outputColl.drop();
    if (outputSharded) {
        st.adminCommand({shardCollection: outputColl.getFullName(), key: {_id: "hashed"}});
    }

    function runMRTestWithOutput(outOptions) {
        assert.commandWorked(inputColl.mapReduce(mapFn, reduceFn, outOptions));
    }

    runMRTestWithOutput(
        {out: Object.assign({merge: outputColl.getName()}, outputSharded ? {sharded: true} : {})});

    assert.commandWorked(outputColl.remove({}));
    runMRTestWithOutput(
        {out: Object.assign({reduce: outputColl.getName()}, outputSharded ? {sharded: true} : {})});
    // Test the same thing using runCommand directly.
    assert.commandWorked(testDB.runCommand({
        mapReduce: inputColl.getName(),
        map: mapFn,
        reduce: reduceFn,
        out: Object.assign({reduce: outputColl.getName()}, outputSharded ? {sharded: true} : {})
    }));

    const output = inputColl.mapReduce(mapFn, reduceFn, {out: {inline: 1}});
    assert.commandWorked(output);
    assert(output.results != 'undefined', "no results for inline");

    if (!outputSharded) {
        // We don't support replacing an existing sharded collection.
        runMRTestWithOutput(outputColl.getName());
        runMRTestWithOutput({out: {replace: outputColl.getName()}});
        runMRTestWithOutput({out: {replace: outputColl.getName(), db: "mrShardOtherDB"}});
        assert.commandWorked(testDB.runCommand({
            mapReduce: inputColl.getName(),
            map: mapFn,
            reduce: reduceFn,
            out: {replace: outputColl.getName()}
        }));
    }
}

testMrOutput({inputSharded: false, outputSharded: false});
testMrOutput({inputSharded: false, outputSharded: true});
testMrOutput({inputSharded: true, outputSharded: false});
testMrOutput({inputSharded: true, outputSharded: true});

// Ensure that mapReduce with a sharded input collection can accept the collation option.
let output = inputColl.mapReduce(mapFn, reduceFn, {out: {inline: 1}, collation: {locale: "en_US"}});
assert.commandWorked(output);
assert(output.results != 'undefined', "no results for inline with collation");

assert.commandWorked(inputColl.remove({}));

// Ensure that the collation option is propagated to the shards. This uses a case-insensitive
// collation, and the query seeding the mapReduce should only match the document if the
// collation is passed along to the shards.
assert.eq(inputColl.find().itcount(), 0);
assert.commandWorked(inputColl.insert({key: 0, value: 0, str: "FOO"}));
output = inputColl.mapReduce(
    mapFn,
    reduceFn,
    {out: {inline: 1}, query: {str: "foo"}, collation: {locale: "en_US", strength: 2}});
assert.commandWorked(output);
assert.eq(output.results, [{_id: 0, value: 1}]);

st.stop();
})();
