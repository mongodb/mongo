//
// Test for what happens when config servers are down and the database config is loaded
// Should fail sanely
//
(function() {
    "use strict";

    var st = new ShardingTest({shards: 1, mongos: 1});

    var mongos = st.s;
    var coll = mongos.getCollection("foo.bar");

    jsTestLog("Stopping config servers");
    for (var i = 0; i < st._configServers.length; i++) {
        MongoRunner.stopMongod(st._configServers[i]);
    }

    // Make sure mongos has no database info currently loaded
    mongos.getDB("admin").runCommand({flushRouterConfig: 1});

    jsTestLog("Config flushed and config servers down!");

    // Throws transport error first and subsequent times when loading config data, not no primary
    for (var i = 0; i < 2; i++) {
        try {
            coll.findOne();
            // Should always throw
            assert(false);
        } catch (e) {
            printjson(e);

            // Make sure we get a transport error, and not a no-primary error
            assert(e.code == 8002 ||   // SCCC config down, for v3.0 compatibility.
                   e.code == 10276 ||  // Transport error
                   e.code == 13328 ||  // Connect error
                   e.code == ErrorCodes.HostUnreachable ||
                   e.code == ErrorCodes.FailedToSatisfyReadPreference ||
                   e.code == ErrorCodes.ReplicaSetNotFound);
        }
    }

    st.stop();

}());
