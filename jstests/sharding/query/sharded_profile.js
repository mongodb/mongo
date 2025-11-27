// Tests whether profiling can trigger stale config errors and interfere with write batches
// SERVER-13413

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {isUweEnabled} from "jstests/libs/query/uwe_utils.js";

let st = new ShardingTest({shards: 1, mongos: 2});
st.stopBalancer();

let admin = st.s0.getDB("admin");
let coll = st.s0.getCollection("foo.bar");

assert(admin.runCommand({enableSharding: coll.getDB() + ""}).ok);
assert(admin.runCommand({shardCollection: coll + "", key: {_id: 1}}).ok);

st.printShardingStatus();

jsTest.log("Turning on profiling on " + st.shard0);

st.shard0.getDB(coll.getDB().toString()).setProfilingLevel(2);

let profileColl = st.shard0.getDB(coll.getDB().toString()).system.profile;

let inserts = [{_id: 0}, {_id: 1}, {_id: 2}];

assert.commandWorked(st.s1.getCollection(coll.toString()).insert(inserts));

const uweEnabled = isUweEnabled(st.s0);
let profileEntry = profileColl.findOne({
    "op": uweEnabled ? "bulkWrite" : "insert",
    "ns": coll.getFullName(),
});
assert.neq(null, profileEntry, profileColl.find().toArray());
printjson(profileEntry);
assert.eq(profileEntry.command.documents, inserts);

st.stop();
