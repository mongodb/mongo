/**
 * Tests the basics of the "directShardConnections" serverStatus metrics.
 *
 * @tags: [requires_fcv_73]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";

function assertNoDirectShardConnectionsMetrics(conn) {
    const res = conn.adminCommand({serverStatus: 1});
    assert(!res.hasOwnProperty("directShardConnections"), res);
}

function assertDirectShardConnectionsMetrics(conn, expected) {
    const res = conn.adminCommand({serverStatus: 1});
    assert(res.hasOwnProperty("directShardConnections"), res);
    assert.eq(res.directShardConnections.current,
              expected.current,
              {expected, actual: res.directShardConnections});
    assert.eq(res.directShardConnections.totalCreated,
              expected.totalCreated,
              {expected, actual: res.directShardConnections});
}

function assertSoonDirectShardConnectionsMetrics(conn, expected) {
    let numTries = 0;
    assert.soon(() => {
        numTries++;
        const res = conn.adminCommand({serverStatus: 1});
        assert(res.hasOwnProperty("directShardConnections"), res);
        if (res.directShardConnections.current != expected.current ||
            res.directShardConnections.totalCreated != expected.totalCreated) {
            if (numTries % 100 == 0) {
                jsTest.log("Still waiting for direct shard connections metrics " +
                           tojson({expected, actual: res.directShardConnections}));
            }
            return false;
        }
        return true;
    });
}

const st = new ShardingTest({mongos: 1, shards: 1});
const shard0Primary = st.rs0.getPrimary();

const dbName = "testDb";
const collName = "testColl";
const ns = dbName + "." + collName;

jsTest.log("Get the initial metrics");
assertNoDirectShardConnectionsMetrics(st.s);
const res = shard0Primary.adminCommand({serverStatus: 1});
assert(res.hasOwnProperty("directShardConnections"), res);
const currentDirectShardConnections = res.directShardConnections;

jsTest.log("Test metrics after creating a new internal connection");
// The mongos will need to create internal connections to shard0's primary to do the write.
assert.commandWorked(st.s.getCollection(ns).insert({_id: 1}));
assertNoDirectShardConnectionsMetrics(st.s);
assertDirectShardConnectionsMetrics(shard0Primary, currentDirectShardConnections);

jsTest.log("Test metrics after reusing on an existing external connection");
assert.neq(shard0Primary.getCollection(ns).findOne({_id: 1}), null);
assertNoDirectShardConnectionsMetrics(st.s);
assertDirectShardConnectionsMetrics(shard0Primary, currentDirectShardConnections);

jsTest.log("Test metrics after creating a new connection");
const newShard0Primary = new Mongo(shard0Primary.host);
assert.neq(newShard0Primary.getCollection(ns).findOne({_id: 1}), null);
currentDirectShardConnections.current++;
currentDirectShardConnections.totalCreated++;
assertNoDirectShardConnectionsMetrics(st.s);
assertDirectShardConnectionsMetrics(newShard0Primary, currentDirectShardConnections);

jsTest.log("Test metrics after creating another new external connection");
const runInsertCommand = (host, dbName, collName) => {
    const conn = new Mongo(host);
    const ns = dbName + "." + collName;
    assert.commandWorked(conn.getCollection(ns).insert({_id: 2}));
    conn.close();
};

const fp = configureFailPoint(shard0Primary, "hangInsertBeforeWrite", {ns});
const insertThread = new Thread(runInsertCommand, shard0Primary.host, dbName, collName);
insertThread.start();
fp.wait();

currentDirectShardConnections.totalCreated++;
currentDirectShardConnections.current++;
assertNoDirectShardConnectionsMetrics(st.s);
assertDirectShardConnectionsMetrics(shard0Primary, currentDirectShardConnections);

fp.off();
insertThread.join();

jsTest.log("Test metrics after closing the new external connection");
currentDirectShardConnections.current--;
assertNoDirectShardConnectionsMetrics(st.s);
// Use assert.soon since there can be a lag for when the connection is destroyed on the server side.
assertSoonDirectShardConnectionsMetrics(shard0Primary, currentDirectShardConnections);

// TODO (SERVER-79353): Connect to shard0's primary on the router port and verify that
// its serverStatus metrics do not contain "directShardConnections" metrics, and that the
// "directShardConnections" metrics on the shard port does not count this connection.

// jsTest.log("Test metrics after creating a external connection on the router port").
// const shard0PrimaryRouter = ...;
// assert.commandWorked(shard0PrimaryRouter.getCollection(ns).insert({_id: 3}));
// assertNoDirectShardConnectionsMetrics(st.s);
// assertNoDirectShardConnectionsMetrics(shard0PrimaryRouter);
// assertDirectShardConnectionsMetrics(shard0Primary, currentDirectShardConnections);

st.stop();
