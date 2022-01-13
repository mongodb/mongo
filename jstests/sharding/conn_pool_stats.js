/**
 * Tests for the connPoolStats command.
 *
 * @tags: [requires_fcv_53]
 */
load("jstests/libs/fail_point_util.js");

(function() {
"use strict";

// Create a cluster with 2 shards.
var cluster = new ShardingTest({shards: 2});

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

assert("pools" in stats);
assert("totalRefreshing" in stats);
assert.lte(stats["totalInUse"] + stats["totalAvailable"] + stats["totalRefreshing"],
           stats["totalCreated"],
           tojson(stats));

assert("totalRefreshed" in stats);

// Enable the following fail point to refresh connections after every command.
var refreshConnectionFailPoint =
    configureFailPoint(cluster.s.getDB("admin"), "refreshConnectionAfterEveryCommand");

var latestTotalRefreshed = stats["totalRefreshed"];

// Iterate over totalRefreshed stat to assert it increases over time.
const totalIterations = 5;
for (var counter = 1; counter <= totalIterations; ++counter) {
    jsTest.log("Testing totalRefreshed, iteration " + counter);
    // Issue a find command to force a connection refresh
    cluster.s.getDB("admin").runCommand({"find": "hello world"});
    assert.soon(function() {
        stats = cluster.s.getDB("admin").runCommand({connPoolStats: 1});
        assert.commandWorked(stats);
        const result = latestTotalRefreshed < stats["totalRefreshed"];
        jsTest.log(tojson({
            "latest total refreshed": latestTotalRefreshed,
            "current total refreshed": stats["totalRefreshed"]
        }));
        latestTotalRefreshed = stats["totalRefreshed"];
        return result;
    }, "totalRefreshed stat did not increase within the expected time", 5000);
}

refreshConnectionFailPoint.off();

cluster.stop();
})();
