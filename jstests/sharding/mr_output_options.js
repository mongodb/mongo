// Tests that the mapReduce command works correctly under all combinations of the input and output
// collections being sharded or unsharded.
(function() {
"use strict";

const st = new ShardingTest({shards: 2, mongos: 1, other: {chunkSize: 1, enableBalancer: true}});

const testDB = st.getDB("mrShard");
const inputColl = testDB.srcSharded;

st.adminCommand({enableSharding: testDB.getName()});
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

function verifyOutput(mrOutput) {
    assert.commandWorked(mrOutput);
    const nTotalDocs = nDistinctKeys * nValuesPerKey;
    assert.eq(mrOutput.counts.input, nTotalDocs, `input count is wrong: ${tojson(mrOutput)}`);
    assert.eq(mrOutput.counts.emit, nTotalDocs, `emit count is wrong: ${tojson(mrOutput)}`);
    assert.gt(
        mrOutput.counts.reduce, nValuesPerKey - 1, `reduce count is wrong: ${tojson(mrOutput)}`);
    assert.eq(mrOutput.counts.output, nDistinctKeys, `output count is wrong: ${tojson(mrOutput)}`);
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
        verifyOutput(inputColl.mapReduce(mapFn, reduceFn, outOptions));
    }

    runMRTestWithOutput({out: {merge: outputColl.getName(), sharded: outputSharded}});

    assert.commandWorked(outputColl.remove({}));
    runMRTestWithOutput({out: {reduce: outputColl.getName(), sharded: outputSharded}});
    // Test the same thing using runCommand directly.
    verifyOutput(testDB.runCommand({
        mapReduce: inputColl.getName(),
        map: mapFn,
        reduce: reduceFn,
        out: {reduce: outputColl.getName(), sharded: outputSharded}
    }));

    const out = inputColl.mapReduce(mapFn, reduceFn, {out: {inline: 1}});
    verifyOutput(out);
    assert(out.results != 'undefined', "no results for inline");

    if (!outputSharded) {
        // We don't support replacing an existing sharded collection.
        runMRTestWithOutput(outputColl.getName());
        runMRTestWithOutput({out: {replace: outputColl.getName(), sharded: outputSharded}});
        runMRTestWithOutput(
            {out: {replace: outputColl.getName(), sharded: outputSharded, db: "mrShardOtherDB"}});
        verifyOutput(testDB.runCommand({
            mapReduce: inputColl.getName(),
            map: mapFn,
            reduce: reduceFn,
            out: {replace: outputColl.getName(), sharded: outputSharded}
        }));
    }
}

testMrOutput({inputSharded: false, outputSharded: false});
testMrOutput({inputSharded: false, outputSharded: true});
testMrOutput({inputSharded: true, outputSharded: false});
testMrOutput({inputSharded: true, outputSharded: true});

// Ensure that mapReduce with a sharded input collection can accept the collation option.
let out = inputColl.mapReduce(mapFn, reduceFn, {out: {inline: 1}, collation: {locale: "en_US"}});
verifyOutput(out);
assert(out.results != 'undefined', "no results for inline with collation");

assert.commandWorked(inputColl.remove({}));

// Ensure that the collation option is propagated to the shards. This uses a case-insensitive
// collation, and the query seeding the mapReduce should only match the document if the
// collation is passed along to the shards.
assert.eq(inputColl.find().itcount(), 0);
assert.commandWorked(inputColl.insert({key: 0, value: 0, str: "FOO"}));
out = inputColl.mapReduce(
    mapFn,
    reduceFn,
    {out: {inline: 1}, query: {str: "foo"}, collation: {locale: "en_US", strength: 2}});
assert.commandWorked(out);
assert.eq(out.counts.input, 1);
assert.eq(out.results, [{_id: 0, value: 1}]);

st.stop();
})();
