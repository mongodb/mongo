// Tests that an aggregate with an $out cannot output to a sharded collection, even if the
// collection becomes sharded during the aggregation.
import {assertErrorCode} from "jstests/aggregation/extras/utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

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
assert.throwsWithCode(() => sourceColl.explain().aggregate([{$out: targetColl.getName()}]), 28769);

// Then test that the $out fails if the collection becomes sharded between establishing the
// cursor and performing the $out.
targetColl.drop();
const cursorResponse = assert.commandWorked(
    mongosDB.runCommand({
        aggregate: sourceColl.getName(),
        pipeline: [{$out: targetColl.getName()}],
        cursor: {batchSize: 0},
    }),
);
st.shardColl(targetColl, {_id: 1}, false);
assert.throwsWithCode(() => new DBCommandCursor(mongosDB, cursorResponse).itcount(), ErrorCodes.IllegalOperation);

st.stop();
