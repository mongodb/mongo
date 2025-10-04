// Test that dropping the config database is completely disabled via
// mongos and via mongod, if started with --configsvr
//
// @tags: [
// ]

import {ShardingTest} from "jstests/libs/shardingtest.js";

// TODO (SERVER-88675): DDL commands against config and admin database are not allowed via a router
// but are allowed via a direct connection to the config server or shard.
TestData.replicaSetEndpointIncompatible = true;

let st = new ShardingTest({shards: 1});
let mongos = st.s;
var config = st.configRS.getPrimary().getDB("config");

jsTest.log("Dropping a collection in admin/config DB is illegal");
{
    assert.commandFailedWithCode(st.s.getDB("admin").runCommand({drop: "secrets"}), ErrorCodes.IllegalOperation);
    assert.commandFailedWithCode(st.s.getDB("config").runCommand({drop: "settings"}), ErrorCodes.IllegalOperation);
}

// Try to drop config db via configsvr

print("1: Try to drop config database via configsvr");
assert.eq(0, config.dropDatabase().ok);
assert.eq("Cannot drop 'config' database if mongod started with --configsvr", config.dropDatabase().errmsg);

// Try to drop config db via mongos
var config = mongos.getDB("config");

print("1: Try to drop config database via mongos");
assert.commandFailedWithCode(config.dropDatabase(), ErrorCodes.IllegalOperation);
assert.commandFailedWithCode(mongos.getDB("admin").dropDatabase(), ErrorCodes.IllegalOperation);

st.stop();
