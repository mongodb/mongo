/**
 * Test to verify 'scope' parameter of mapReduce command. This test verfies that 'map', 'reduce' and
 * 'finalize' functions can use 'scope' variable passed in the input.
 * @tags: [
 *   requires_scripting,
 * ]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 2});
const dbName = jsTest.name();
const coll = st.s.getDB(dbName).coll;
st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName});

function runTest(coll) {
    /* eslint-disable */
    const map = function() {
        emit(xx.val, this.a);
    };
    const reduce = function(key, values) {
        return {reduce: xx.val + 1};
    };
    const finalize = function(key, values) {
        values.finalize = xx.val + 2;
        return values;
    };
    /* eslint-enable */
    const res = assert.commandWorked(
        coll.mapReduce(map, reduce, {finalize: finalize, out: {inline: 1}, scope: {xx: {val: 9}}}));
    assert.eq(res.results.length, 1, res);
    assert.eq(res.results[0], {_id: 9, value: {reduce: 10, finalize: 11}}, res);
}

assert.commandWorked(coll.insert({}));

// Run test when a single shard is targetted.
runTest(coll);

// Run test when more than one shard is targetted.
st.shardColl("coll", {a: 1}, {a: 0});
runTest(coll);

st.stop();
