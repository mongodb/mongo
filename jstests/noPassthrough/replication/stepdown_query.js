/**
 * Tests that a query with default read preference ("primary") will succeed even if the node being
 * queried steps down before the final result batch has been delivered.
 * @tags: [
 *   requires_replication,
 *   requires_sharding,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// Checking UUID consistency involves talking to a shard node, which in this test is shutdown
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;
TestData.skipCheckOrphans = true;

// Set the refresh period to 10 min to rule out races
_setShellFailPoint({
    configureFailPoint: "modifyReplicaSetMonitorDefaultRefreshPeriod",
    mode: "alwaysOn",
    data: {
        period: 10 * 60,
    },
});

let dbName = "test";
let collName = jsTest.name();

function runTest(host, rst, waitForPrimary) {
    // We create a new connection to 'host' here instead of passing in the original connection.
    // This to work around the fact that connections created by ReplSetTest already have secondaryOk
    // set on them, but we need a connection with secondaryOk not set for this test.
    let conn = new Mongo(host);
    let coll = conn.getDB(dbName).getCollection(collName);
    assert(!coll.exists());
    assert.commandWorked(coll.insert([{}, {}, {}, {}, {}]));
    let cursor = coll.find().batchSize(2);
    // Retrieve the first batch of results.
    cursor.next();
    cursor.next();
    assert.eq(0, cursor.objsLeftInBatch());
    let primary = rst.getPrimary();
    let secondary = rst.getSecondary();
    assert.commandWorked(primary.getDB("admin").runCommand({replSetStepDown: 60, force: true}));
    rst.awaitSecondaryNodes(null, [primary]);
    if (waitForPrimary) {
        rst.waitForState(secondary, ReplSetTest.State.PRIMARY);
    }
    // When the primary steps down, it closes all client connections. Since 'conn' may be a
    // direct connection to the primary and the shell doesn't automatically retry operations on
    // network errors, we run a dummy operation here to force the shell to reconnect.
    try {
        conn.getDB("admin").runCommand("ping");
    } catch (e) {}

    // Even though our connection doesn't have secondaryOk set, we should still be able to iterate
    // our cursor and kill our cursor.
    assert(cursor.hasNext());
    assert.doesNotThrow(function () {
        cursor.close();
    });
}

// Test querying a replica set primary directly.
let rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate(null, null, {initiateWithDefaultElectionTimeout: true});
runTest(rst.getPrimary().host, rst, false);
rst.stopSet();

rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate(null, null, {initiateWithDefaultElectionTimeout: true});
runTest(rst.getURL(), rst, true);
rst.stopSet();

// Test querying a replica set primary through mongos.
let st = new ShardingTest({
    shards: 1,
    rs: {nodes: 2},
    config: 2,
    // By default, our test infrastructure sets the election timeout to a very high value (24
    // hours). For this test, we need a shorter election timeout because it relies on nodes running
    // an election when they do not detect an active primary. Therefore, we are setting the
    // electionTimeoutMillis to its default value.
    initiateWithDefaultElectionTimeout: true,
});
rst = st.rs0;
runTest(st.s0.host, rst, true);
st.stop();
