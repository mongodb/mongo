/**
 * Test ClusterFindCmd with UUID for collection name fails (but does not crash)
 */
(function() {
"use strict";

var cmdRes;
var cursorId;

var st = new ShardingTest({shards: 2});
st.stopBalancer();

var db = st.s.getDB("test");

assert.commandWorked(db.adminCommand({enableSharding: db.getName()}));

cmdRes = db.adminCommand({find: UUID()});
assert.commandFailed(cmdRes);

st.stop();
})();
