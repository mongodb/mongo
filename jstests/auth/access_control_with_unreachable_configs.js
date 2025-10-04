// Validates that, when it cannot reach a config server, mongos assumes that the
// localhost exception does not apply.  That is, if mongos cannot verify that there
// are user documents stored in the configuration information, it must assume that
// there are.
// @tags: [requires_sharding]

import {ShardingTest} from "jstests/libs/shardingtest.js";

// The config servers are not reachable at shutdown.
TestData.skipCheckingIndexesConsistentAcrossCluster = true;
TestData.skipCheckOrphans = true;
TestData.skipCheckShardFilteringMetadata = true;

let st = new ShardingTest({
    shards: 1,
    mongos: 1,
    config: 1,
    keyFile: "jstests/libs/key1",
    useHostname: false, // Needed when relying on the localhost exception
    other: {mongosOptions: {verbose: 1}},
});
let mongos = st.s;
let config = st.config0;
let authzErrorCode = 13;

// set up user/pwd on admin db with clusterAdmin role (for serverStatus)
let conn = new Mongo(mongos.host);
var db = conn.getDB("admin");
db.createUser({user: "user", pwd: "pwd", roles: ["clusterAdmin"]});
db.auth("user", "pwd");

// open a new connection to mongos (unauthorized)
conn = new Mongo(mongos.host);
db = conn.getDB("admin");

// first serverStatus should fail since user is not authorized
assert.commandFailedWithCode(db.adminCommand("serverStatus"), authzErrorCode);

// authorize and repeat command, works
db.auth("user", "pwd");
assert.commandWorked(db.adminCommand("serverStatus"));

jsTest.log("repeat without config server");

// shut down only config server
MongoRunner.stopMongod(config);

// open a new connection to mongos (unauthorized)
let conn2 = new Mongo(mongos.host);
let db2 = conn2.getDB("admin");

// should fail since user is not authorized.
assert.commandFailedWithCode(db2.adminCommand("serverStatus"), authzErrorCode);
st.stop();
