/**
 * Test that merging the explain results from shards resulting in over 16MB response throws the
 * 'BSONObjectTooLarge' error code.
 *
 * @tags: [
 *   requires_fcv_82,
 * ]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 2});
const mongosDB = st.s.getDB("test");
const coll = mongosDB.agg_explain_fmt;

// Now shard the collection by _id and move a chunk to each shard.
st.shardColl(coll, {_id: 1}, {_id: 0}, {_id: 0});

{
    assert.commandWorked(coll.insert({groupBy: 1, largeField: "a".repeat(1000)}));
    assert.commandWorked(coll.insert({groupBy: 2, largeField: "a".repeat(1000)}));
    const largeAccumulator = {
        $accumulator: {
            init: function() {
                return "";
            },
            accumulateArgs: [{fieldName: "$a"}],
            accumulate: function(state, args) {
                return state + "a";
            },
            merge: function(state1, state2) {
                return state1 + state2;
            },
            finalize: function(state) {
                return state.length;
            }
        }
    };
    assert.commandWorked(mongosDB.runCommand({
        explain: {
            aggregate: coll.getName(),
            pipeline: [
                {$addFields: {a: {$range: [0, 1000000]}}},
                {$unwind: "$a"},  // Create a number of documents to be executed by the accumulator.
                {$group: {_id: "$groupBy", count: largeAccumulator}}
            ],
            cursor: {}
        }
    }));
}

st.stop();
