/**
 * Test verifying that the system variables are resolved before sent to the shards.
 *
 * Always ensure that there are enough shards in the pool to guarantee non-synced time resolution on
 * shard side.
 *
 * @tags: [
 * requires_fcv_80
 * ]
 *
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

const numIterations = 5;

const st = new ShardingTest({shards: 5});
const db = st.s.getDB("find_initialize_runtime_variables");
st.s.adminCommand({shardCollection: "find_initialize_runtime_variables.c", key: {a: "hashed"}});
let coll = db.getCollection("find_initialize_runtime_variables");

coll.insert([{a: 1}, {a: 2}, {a: 3}]);

for (let i = 0; i < numIterations; ++i) {
    let res = coll.find({}, {a: 1, b: "$$NOW"}).toArray();
    for (const e of res) {
        // Check consistency of the returned $$NOW values.
        assert.eq(res[0].b, e.b);
    }

    res = coll.find({}, {a: 1, b: "$$CLUSTER_TIME"}).toArray();
    for (const e of res) {
        // Check consistency of the returned $$CLUSTER_TIME values.
        assert.eq(res[0].b, e.b);
    }
}

st.stop();
