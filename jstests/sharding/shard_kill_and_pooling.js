/**
 * Tests what happens when a shard goes down with pooled connections.
 *
 * This test involves restarting a standalone shard, so cannot be run on ephemeral storage engines.
 * A restarted standalone will lose all data when using an ephemeral storage engine.
 *
 * MongoD cannot downgrade after a hard crash, or restart from standalone.
 *
 * @tags: [multiversion_incompatible,requires_persistence]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

// Checking UUID consistency involves talking to a shard node, which in this test is shutdown
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

// Run through the same test twice, once with a hard -9 kill, once with a regular shutdown
for (let test = 0; test < 2; test++) {
    let killWith = test == 0 ? 15 : 9;

    let st = new ShardingTest({shards: 1});

    let mongos = st.s0;
    let coll = mongos.getCollection("foo.bar");

    assert.commandWorked(coll.insert({hello: "world"}));

    jsTest.log("Creating new connections...");

    // Create a bunch of connections to the primary node through mongos.
    // jstest ->(x10)-> mongos ->(x10)-> primary
    let conns = [];
    for (let i = 0; i < 50; i++) {
        conns.push(new Mongo(mongos.host));
        assert.neq(null, conns[i].getCollection(coll + "").findOne());
    }

    jsTest.log("Returning the connections back to the pool.");

    for (let i = 0; i < conns.length; i++) {
        conns[i].close();
    }

    // Log connPoolStats for debugging purposes.
    let connPoolStats = mongos.getDB("admin").runCommand({connPoolStats: 1});
    printjson(connPoolStats);

    jsTest.log("Shutdown shard " + (killWith == 9 ? "uncleanly" : "") + "...");

    // Flush writes to disk, since sometimes we're killing uncleanly
    assert(mongos.getDB("admin").runCommand({fsync: 1}).ok);

    let exitCode = killWith === 9 ? MongoRunner.EXIT_SIGKILL : MongoRunner.EXIT_CLEAN;

    for (let node of st.rs0.nodes) {
        st.rs0.stop(st.rs0.getNodeId(node), killWith, {allowedExitCode: exitCode}, {forRestart: true});
    }
    jsTest.log("Restart shard...");
    st.rs0.startSet({forceLock: true}, true);

    jsTest.log("Waiting for socket timeout time...");

    // Need to wait longer than the socket polling time.
    sleep(2 * 5000);

    jsTest.log("Run queries using new connections.");

    let numErrors = 0;
    for (let i = 0; i < conns.length; i++) {
        let newConn = new Mongo(mongos.host);
        try {
            assert.neq(null, newConn.getCollection("foo.bar").findOne());
        } catch (e) {
            printjson(e);
            numErrors++;
        }
    }

    assert.eq(0, numErrors);

    st.stop();

    jsTest.log("DONE test " + test);
}
