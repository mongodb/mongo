// Tests that an aggregate with an $out cannot output to a sharded collection, even if the
// collection becomes sharded during the aggregation.
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For 'assertErrorCode'.

const st = new ShardingTest({shards: 2});

const mongosDB = st.s.getDB("test");
const sourceColl = mongosDB.source;
const targetColl = mongosDB.target;

assert.commandWorked(sourceColl.insert(Array.from({length: 10}, (_, i) => ({_id: i}))));

// First simply test that the $out fails if the target collection is definitely sharded, meaning
// it starts as sharded and remains sharded for the duration of the $out.
st.shardColl(targetColl, {_id: 1}, false);
assertErrorCode(sourceColl, [{$out: targetColl.getName()}], 28769);

// Test that the "legacy" mode will not succeed when outputting to a sharded collection, even
// for explain.
let error = assert.throws(() => sourceColl.explain().aggregate([{$out: targetColl.getName()}]));
assert.eq(error.code, 28769);

// Then test that the $out fails if the collection becomes sharded between establishing the
// cursor and performing the $out.
targetColl.drop();
const cursorResponse = assert.commandWorked(mongosDB.runCommand({
    aggregate: sourceColl.getName(),
    pipeline: [{$out: targetColl.getName()}],
    cursor: {batchSize: 0}
}));
st.shardColl(targetColl, {_id: 1}, false);
error = assert.throws(() => new DBCommandCursor(mongosDB, cursorResponse).itcount());
// On master, we check whether the output collection is sharded at parse time so this error code
// is simply 'CommandFailed' because it is a failed rename going through the DBDirectClient. The
// message should indicate that the rename failed. In a mixed-version environment we can end up
// with the code 17017 because a v4.0 shard will assert the collection is unsharded before
// performing any writes but after parse time, instead of relying on the rename to fail. Because
// this test is run in a mixed-version passthrough we have to allow both. Once 4.2 becomes the
// last stable version, this assertion can be tightened up to only expect CommandFailed.
assert.contains(error.code, [ErrorCodes.CommandFailed, 17017]);

st.stop();
}());
