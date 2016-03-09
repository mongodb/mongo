// Tests for the connPoolStats command.

// Create a cluster with 2 shards.
var cluster = new ShardingTest({shards: 2});

// Run the connPoolStats command
stats = cluster.s.getDB("admin").runCommand({connPoolStats: 1});

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
