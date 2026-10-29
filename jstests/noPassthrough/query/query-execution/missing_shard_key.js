/**
 * Verify that a query requiring shard filtering handles missing shard keys properly.
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({
    shards: 1,
});

const dbName = "foo";
const ns = "foo.bar";

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {skey: 1}}));

assert.commandWorked(st.s.getCollection(ns).insert({_id: 1, x: 1}));
assert.sameMembers(st.s.getCollection(ns).find({x: 1}).toArray(), [{_id: 1, x: 1}]);
st.stop();
