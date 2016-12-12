// Tests for the connPoolStats command.
(function() {
    "use strict";
    // Create a cluster with 2 shards.
    var cluster = new ShardingTest({shards: 2});

    // Needed because the command was expanded post 3.2
    var version = cluster.s.getDB("admin").runCommand({buildinfo: 1}).versionArray;
    var post32 = (version[0] > 4) || ((version[0] == 3) && (version[1] > 2));

    // Run the connPoolStats command
    var stats = cluster.s.getDB("admin").runCommand({connPoolStats: 1});

    // Validate output
    printjson(stats);
    assert.commandWorked(stats);
    assert("replicaSets" in stats);
    assert("hosts" in stats);
    assert("numClientConnections" in stats);
    assert("numAScopedConnections" in stats);
    assert("totalInUse" in stats);
    assert("totalAvailable" in stats);
    assert("totalCreated" in stats);
    assert.lte(stats["totalInUse"] + stats["totalAvailable"], stats["totalCreated"], tojson(stats));
    if (post32) {
        assert("pools" in stats);
        assert("totalRefreshing" in stats);
        assert.lte(stats["totalInUse"] + stats["totalAvailable"] + stats["totalRefreshing"],
                   stats["totalCreated"],
                   tojson(stats));
    }
})();
