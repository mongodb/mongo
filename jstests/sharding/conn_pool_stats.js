// Tests for the connPoolStats command.

// Create a cluster with 2 shards.
var cluster = new ShardingTest({shards: 2});

// Run the connPoolStats command
stats = cluster.s.getDB("admin").runCommand({connPoolStats : 1});

// Validate output
printjson(stats);
assert.commandWorked(stats);
assert("replicaSets" in stats);

assert("pools" in stats);
var pools = stats["pools"];

// Stats from dbclient pool
assert("DBClient (Global)" in pools);
var dbclient = pools["DBClient (Global)"];
assert("hosts" in dbclient);
assert("numClientConnection" in dbclient);
assert("numAScopedConnection" in dbclient);
assert("totalInUse" in dbclient);
assert("totalAvailable" in dbclient);
assert("totalCreated" in dbclient);
assert.eq(dbclient["totalInUse"] + dbclient["totalAvailable"], dbclient["totalCreated"]);

// Stats from ASIO pool
assert("NetworkInterfaceASIO (Sharding)" in pools);
var asio = pools["NetworkInterfaceASIO (Sharding)"];
assert("hosts" in asio);
assert("totalInUse" in asio);
assert("totalAvailable" in asio);
assert("totalCreated" in asio);
assert.eq(asio["totalInUse"] + asio["totalAvailable"], asio["totalCreated"]);
