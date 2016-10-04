//
// Tests what happens when a replica set primary goes down with pooled connections.
//
(function() {
    "use strict";

    var st = new ShardingTest({shards: {rs0: {nodes: 2}}, mongos: 1});

    // Stop balancer to eliminate weird conn stuff
    st.stopBalancer();

    var mongos = st.s0;
    var coll = mongos.getCollection("foo.bar");
    var db = coll.getDB();

    // Test is not valid for Win32
    var is32Bits = (db.serverBuildInfo().bits == 32);
    if (is32Bits && _isWindows()) {
        // Win32 doesn't provide the polling interface we need to implement the check tested here
        jsTest.log("Test is not valid on Win32 platform.");

    } else {
        // Non-Win32 platform

        var primary = st.rs0.getPrimary();
        var secondary = st.rs0.getSecondary();

        jsTest.log("Creating new connections...");

        // Create a bunch of connections to the primary node through mongos.
        // jstest ->(x10)-> mongos ->(x10)-> primary
        var conns = [];
        for (var i = 0; i < 50; i++) {
            conns.push(new Mongo(mongos.host));
            conns[i].getCollection(coll + "").findOne();
        }

        jsTest.log("Returning the connections back to the pool.");

        for (var i = 0; i < conns.length; i++) {
            conns[i] = null;
        }
        // Make sure we return connections back to the pool
        gc();

        // Don't make test fragile by linking to format of shardConnPoolStats, but this is useful if
        // something goes wrong.
        var connPoolStats = mongos.getDB("admin").runCommand({shardConnPoolStats: 1});
        printjson(connPoolStats);

        jsTest.log("Stepdown primary and then step back up...");

        var stepDown = function(node, timeSecs) {
            var result = null;
            try {
                result = node.getDB("admin").runCommand({replSetStepDown: timeSecs, force: true});
                // Should not get here
            } catch (e) {
                printjson(e);
            }

            if (result != null)
                printjson(result);
            assert.eq(null, result);
        };

        stepDown(primary, 0);

        jsTest.log("Waiting for mongos to acknowledge stepdown...");

        ReplSetTest.awaitRSClientHosts(
            mongos,
            secondary,
            {ismaster: true},
            st.rs0,
            2 * 60 * 1000);  // slow hosts can take longer to recognize sd

        jsTest.log("Stepping back up...");

        stepDown(secondary, 10000);

        jsTest.log("Waiting for mongos to acknowledge step up...");

        ReplSetTest.awaitRSClientHosts(mongos, primary, {ismaster: true}, st.rs0, 2 * 60 * 1000);

        jsTest.log("Waiting for socket timeout time...");

        // Need to wait longer than the socket polling time.
        sleep(2 * 5000);

        jsTest.log("Run queries using new connections.");

        var numErrors = 0;
        for (var i = 0; i < conns.length; i++) {
            var newConn = new Mongo(mongos.host);
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
}());