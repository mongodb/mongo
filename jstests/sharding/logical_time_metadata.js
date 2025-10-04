/**
 * Basic test that checks that mongos includes the cluster time metatadata in it's response.
 * This does not test cluster time propagation via the shell as there are many back channels
 * where the cluster time metadata can propagated, making it inherently racy.
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

function assertHasClusterTimeAndOperationTime(res) {
    assert.hasFields(res, ["$clusterTime"]);
    assert.hasFields(res.$clusterTime, ["clusterTime", "signature"]);
}

let st = new ShardingTest({shards: {rs0: {nodes: 3}}});
st.s.adminCommand({enableSharding: "test"});

let db = st.s.getDB("test");

let res = db.runCommand({insert: "user", documents: [{x: 10}]});
assert.commandWorked(res);
assertHasClusterTimeAndOperationTime(res);

res = db.runCommand({blah: "blah"});
assert.commandFailed(res);
assertHasClusterTimeAndOperationTime(res);

res = db.runCommand({insert: "user", documents: [{x: 10}], writeConcern: {blah: "blah"}});
assert.commandFailed(res);
assertHasClusterTimeAndOperationTime(res);

res = st.rs0.getPrimary().adminCommand({replSetGetStatus: 1});

// Cluster time may advance after replSetGetStatus finishes executing and before its logical
// time metadata is computed, in which case the response's $clusterTime will be greater than the
// appliedOpTime timestamp in its body. Assert the timestamp is <= $clusterTime to account for
// this.
let appliedTime = res.optimes.appliedOpTime.ts;
let logicalTimeMetadata = res.$clusterTime;
assert.lte(
    timestampCmp(appliedTime, logicalTimeMetadata.clusterTime),
    0,
    "appliedTime: " +
        tojson(appliedTime) +
        " not less than or equal to clusterTime: " +
        tojson(logicalTimeMetadata.clusterTime),
);

assert.commandWorked(db.runCommand({ping: 1, "$clusterTime": logicalTimeMetadata}));

db = st.rs0.getPrimary().getDB("testRS");
res = db.runCommand({insert: "user", documents: [{x: 10}]});
assert.commandWorked(res);
assertHasClusterTimeAndOperationTime(res);

res = db.runCommand({blah: "blah"});
assert.commandFailed(res);
assertHasClusterTimeAndOperationTime(res);

res = db.runCommand({insert: "user", documents: [{x: 10}], writeConcern: {blah: "blah"}});
assert.commandFailed(res);
assertHasClusterTimeAndOperationTime(res);

st.stop();
