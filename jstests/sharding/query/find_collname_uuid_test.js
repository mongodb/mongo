/**
 * Test ClusterFindCmd with UUID for collection name fails (but does not crash)
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

let cmdRes;
let cursorId;

let st = new ShardingTest({shards: 2});
st.stopBalancer();

var db = st.s.getDB("test");

assert.commandWorked(db.adminCommand({enableSharding: db.getName()}));

cmdRes = db.adminCommand({find: UUID()});
assert.commandFailed(cmdRes);

st.stop();
