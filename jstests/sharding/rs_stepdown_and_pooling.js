//
// Tests what happens when a replica set primary goes down with pooled connections.
//
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {awaitRSClientHosts} from "jstests/replsets/rslib.js";

let st = new ShardingTest({
    shards: {rs0: {nodes: 2}},
    mongos: 1,
    // By default, our test infrastructure sets the election timeout to a very high value (24
    // hours). For this test, we need a shorter election timeout because it relies on nodes running
    // an election when they do not detect an active primary. Therefore, we are setting the
    // electionTimeoutMillis to its default value.
    initiateWithDefaultElectionTimeout: true,
});

// Stop balancer to eliminate weird conn stuff
st.stopBalancer();

let mongos = st.s0;
let coll = mongos.getCollection("foo.bar");
let db = coll.getDB();

// Test is not valid for Win32
let is32Bits = db.getServerBuildInfo().getBits() == 32;
if (is32Bits && _isWindows()) {
    // Win32 doesn't provide the polling interface we need to implement the check tested here
    jsTest.log("Test is not valid on Win32 platform.");
} else {
    // Non-Win32 platform

    let primary = st.rs0.getPrimary();
    let secondary = st.rs0.getSecondary();

    jsTest.log("Creating new connections...");

    // Create a bunch of connections to the primary node through mongos.
    // jstest ->(x10)-> mongos ->(x10)-> primary
    let conns = [];
    for (let i = 0; i < 50; i++) {
        conns.push(new Mongo(mongos.host));
        conns[i].getCollection(coll + "").findOne();
    }

    jsTest.log("Returning the connections back to the pool.");

    for (let i = 0; i < conns.length; i++) {
        conns[i] = null;
    }
    // Make sure we return connections back to the pool
    gc();

    // Log connPoolStats for debugging purposes.
    let connPoolStats = mongos.getDB("admin").runCommand({connPoolStats: 1});
    printjson(connPoolStats);

    jsTest.log("Stepdown primary and then step back up...");

    let stepDown = function(node, timeSecs) {
        assert.commandWorked(
            node.getDB("admin").runCommand({replSetStepDown: timeSecs, force: true}));
    };

    stepDown(primary, 0);

    jsTest.log("Waiting for mongos to acknowledge stepdown...");

    awaitRSClientHosts(mongos,
                       secondary,
                       {ismaster: true},
                       st.rs0,
                       2 * 60 * 1000);  // slow hosts can take longer to recognize sd

    jsTest.log("Stepping back up...");

    stepDown(secondary, 10000);

    jsTest.log("Waiting for mongos to acknowledge step up...");

    awaitRSClientHosts(mongos, primary, {ismaster: true}, st.rs0, 2 * 60 * 1000);

    jsTest.log("Waiting for socket timeout time...");

    // Need to wait longer than the socket polling time.
    sleep(2 * 5000);

    jsTest.log("Run queries using new connections.");

    let numErrors = 0;
    for (let i = 0; i < conns.length; i++) {
        let newConn = new Mongo(mongos.host);
        try {
            printjson(newConn.getCollection("foo.bar").findOne());
        } catch (e) {
            printjson(e);
            numErrors++;
        }
    }

    assert.eq(0, numErrors);
}  // End Win32 check

jsTest.log("DONE!");

st.stop();
