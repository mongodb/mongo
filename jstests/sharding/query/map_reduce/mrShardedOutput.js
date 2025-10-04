// Tests the behavior of mapReduce outputting to a sharded collection and specifying the 'sharded'
// flag.
// This test stresses behavior that is only true of the mapReduce implementation using aggregation,
// so it cannot be run in mixed-version suites.
// @tags: [requires_scripting]
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 2, other: {chunkSize: 1}});

const testDB = st.getDB("test");
const inputColl = testDB.foo;
const outputColl = testDB.mr_sharded_out;

const numDocs = 500;
const str = "a".repeat(1023);
// Shard the input collection by "a" and split into two chunks, one on each shard.
st.shardColl(inputColl, {a: 1}, {a: numDocs / 2}, {a: numDocs});

function map() {
    emit(this._id, {count: 1, y: this.y});
}
function reduce(key, values) {
    // 'values' can contain a null entry if we're using reduce for output mode "reduce" and the
    // target doesn't exist.
    return {
        count: Array.sum(values.map((val) => (val === null ? 0 : val.count))),
        y: values.map((val) => (val === null ? "" : val.y)).join(""),
    };
}

const bulk = inputColl.initializeUnorderedBulkOp();
for (let i = 0; i < numDocs; ++i) {
    bulk.insert({_id: i, a: numDocs + i, y: str, i: numDocs + i});
}
assert.commandWorked(bulk.execute());

// Should not be able to replace to a sharded collection.
assert.throwsWithCode(
    () => inputColl.mapReduce(map, reduce, {out: {replace: outputColl.getName(), sharded: true}}),
    ErrorCodes.InvalidOptions,
);

// Should fail if we specify "merge" or "reduce" with sharded: true and the collection does not yet
// exist as sharded.
assert.throwsWithCode(
    () => inputColl.mapReduce(map, reduce, {out: {merge: outputColl.getName(), sharded: true}}),
    ErrorCodes.InvalidOptions,
);

assert.throwsWithCode(
    () => inputColl.mapReduce(map, reduce, {out: {reduce: outputColl.getName(), sharded: true}}),
    ErrorCodes.InvalidOptions,
);

// Now create and shard the output collection, again with one chunk on each shard.
st.shardColl(outputColl, {_id: 1}, {_id: numDocs / 2}, {_id: numDocs / 2});

// Should be able to output successfully with any non-replace mode now.
inputColl.mapReduce(map, reduce, {out: {merge: outputColl.getName(), sharded: true}});
assert.eq(numDocs, outputColl.find().itcount());
// It should not be required to specify the sharded option anymore.
inputColl.mapReduce(map, reduce, {out: {merge: outputColl.getName()}});
assert.eq(numDocs, outputColl.find().itcount());
assert(FixtureHelpers.isSharded(outputColl));

// Now remove half of the output collection to prove that we can "reduce" with those that exist.
assert.commandWorked(outputColl.remove({_id: {$mod: [2, 0]}}));
inputColl.mapReduce(map, reduce, {out: {reduce: outputColl.getName(), sharded: true}});
assert.eq(numDocs, outputColl.find().itcount());
const evenResult = outputColl.findOne({_id: {$mod: [2, 0]}});
const oddResult = outputColl.findOne({_id: {$mod: [2, 1]}});
assert.eq(evenResult.value.count * 2, oddResult.value.count, [evenResult, oddResult]);

// Should not be able to use replace mode if the collection exists and is sharded, even if the
// 'sharded' option is not specified.
assert.throwsWithCode(
    () => inputColl.mapReduce(map, reduce, {out: {replace: outputColl.getName()}}),
    ErrorCodes.IllegalOperation,
);

st.stop();
