// Test that dropping the config database is completely disabled via
// mongos and via mongod, if started with --configsvr
(function() {
"use strict";

var st = new ShardingTest({shards: 2});
var mongos = st.s;
var config = st.configRS.getPrimary().getDB('config');

// Try to drop config db via configsvr

print("1: Try to drop config database via configsvr");
assert.eq(0, config.dropDatabase().ok);
assert.eq("Cannot drop 'config' database if mongod started with --configsvr",
          config.dropDatabase().errmsg);

// Try to drop config db via mongos
var config = mongos.getDB("config");

print("1: Try to drop config database via mongos");
assert.commandFailedWithCode(config.dropDatabase(), ErrorCodes.IllegalOperation);
assert.commandFailedWithCode(mongos.getDB("admin").dropDatabase(), ErrorCodes.IllegalOperation);

st.stop();
}());
