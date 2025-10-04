//
// Tests mongos's failure tolerance for authenticated replica set shards and slaveOk queries
//
// Sets up a cluster with three shards, the first shard of which has an unsharded collection and
// half a sharded collection.  The second shard has the second half of the sharded collection, and
// the third shard has nothing.  Progressively shuts down the primary of each shard to see the
// impact on the cluster.
//
// Three different connection states are tested - active (connection is active through whole
// sequence), idle (connection is connected but not used before a shard change), and new
// (connection connected after shard change).
//

import {ShardingTest} from "jstests/libs/shardingtest.js";

// The following checks involves talking to shard primaries, but by the end of this test, one shard
// does not have a primary.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;
TestData.skipCheckingIndexesConsistentAcrossCluster = true;
TestData.skipCheckOrphans = true;
TestData.skipCheckShardFilteringMetadata = true;
TestData.skipCheckMetadataConsistency = true;

// Replica set nodes started with --shardsvr do not enable key generation until they are added to a
// sharded cluster and reject commands with gossiped clusterTime from users without the
// advanceClusterTime privilege. This causes ShardingTest setup to fail because the shell briefly
// authenticates as __system and receives clusterTime metadata then will fail trying to gossip that
// time later in setup.
//

// Multiple users cannot be authenticated on one connection within a session.
TestData.disableImplicitSessions = true;

let options = {
    rs: true,
    rsOptions: {nodes: 2},
    keyFile: "jstests/libs/key1",
    // ShardingTest use a high config command timeout to avoid spurious failures but this test may
    // require a timeout to complete, so we restore the default value to avoid failures.
    mongosOptions: {setParameter: {defaultConfigCommandTimeoutMS: 30000}},
};

let st = new ShardingTest({shards: 3, mongos: 1, other: options});

let mongos = st.s0;
let admin = mongos.getDB("admin");

jsTest.log("Setting up initial admin user...");
let adminUser = "adminUser";
let password = "password";

// Create a user
admin.createUser({user: adminUser, pwd: password, roles: ["root"]});
// There's an admin user now, so we need to login to do anything
// Login as admin user
admin.auth(adminUser, password);

st.stopBalancer();

assert.commandWorked(admin.runCommand({setParameter: 1, traceExceptions: true}));

// Create the unsharded database with shard0 primary
assert.commandWorked(st.s.adminCommand({enableSharding: "fooUnsharded", primaryShard: st.shard0.shardName}));
// Create the sharded database with shard1 primary
assert.commandWorked(st.s.adminCommand({enableSharding: "fooSharded", primaryShard: st.shard1.shardName}));

let collSharded = mongos.getCollection("fooSharded.barSharded");
let collUnsharded = mongos.getCollection("fooUnsharded.barUnsharded");

assert.commandWorked(admin.runCommand({shardCollection: collSharded.toString(), key: {_id: 1}}));
assert.commandWorked(admin.runCommand({split: collSharded.toString(), middle: {_id: 0}}));
assert.commandWorked(admin.runCommand({moveChunk: collSharded.toString(), find: {_id: -1}, to: st.shard0.shardName}));

st.printShardingStatus();
const dbUser = "dbUser";

jsTest.log("Setting up database users...");

// Create db user
admin.createUser({user: dbUser, pwd: password, roles: ["readWriteAnyDatabase"]});
admin.logout();

function authDBUsers(conn) {
    assert(conn.getDB("admin").auth(dbUser, password));
    return conn;
}

// Secondaries do not refresh their in-memory routing table until a request with a higher version
// is received, and refreshing requires communication with the primary to obtain the newest version.
// Read from the secondaries once before taking down primaries to ensure they have loaded the
// routing table into memory.
let mongosSetupConn = authDBUsers(new Mongo(mongos.host));
mongosSetupConn.setReadPref("secondary");
assert(!mongosSetupConn.getCollection(collSharded.toString()).find({}).hasNext());

gc(); // Clean up connections

//
// Setup is complete
//

jsTest.log("Inserting initial data...");

let mongosConnActive = authDBUsers(new Mongo(mongos.host));
let mongosConnIdle = null;
let mongosConnNew = null;

let wc = {writeConcern: {w: 2, wtimeout: 60000}};

assert.commandWorked(mongosConnActive.getCollection(collSharded.toString()).insert({_id: -1}, wc));
assert.commandWorked(mongosConnActive.getCollection(collSharded.toString()).insert({_id: 1}, wc));
assert.commandWorked(mongosConnActive.getCollection(collUnsharded.toString()).insert({_id: 1}, wc));

jsTest.log("Stopping primary of third shard...");

mongosConnIdle = authDBUsers(new Mongo(mongos.host));

st.rs2.stop(st.rs2.getPrimary());

jsTest.log("Testing active connection with third primary down...");

assert.neq(null, mongosConnActive.getCollection(collSharded.toString()).findOne({_id: -1}));
assert.neq(null, mongosConnActive.getCollection(collSharded.toString()).findOne({_id: 1}));
assert.neq(null, mongosConnActive.getCollection(collUnsharded.toString()).findOne({_id: 1}));

assert.commandWorked(mongosConnActive.getCollection(collSharded.toString()).insert({_id: -2}, wc));
assert.commandWorked(mongosConnActive.getCollection(collSharded.toString()).insert({_id: 2}, wc));
assert.commandWorked(mongosConnActive.getCollection(collUnsharded.toString()).insert({_id: 2}, wc));

jsTest.log("Testing idle connection with third primary down...");

assert.commandWorked(mongosConnIdle.getCollection(collSharded.toString()).insert({_id: -3}, wc));
assert.commandWorked(mongosConnIdle.getCollection(collSharded.toString()).insert({_id: 3}, wc));
assert.commandWorked(mongosConnIdle.getCollection(collUnsharded.toString()).insert({_id: 3}, wc));

assert.neq(null, mongosConnIdle.getCollection(collSharded.toString()).findOne({_id: -1}));
assert.neq(null, mongosConnIdle.getCollection(collSharded.toString()).findOne({_id: 1}));
assert.neq(null, mongosConnIdle.getCollection(collUnsharded.toString()).findOne({_id: 1}));

jsTest.log("Testing new connections with third primary down...");

mongosConnNew = authDBUsers(new Mongo(mongos.host));
assert.neq(null, mongosConnNew.getCollection(collSharded.toString()).findOne({_id: -1}));
mongosConnNew = authDBUsers(new Mongo(mongos.host));
assert.neq(null, mongosConnNew.getCollection(collSharded.toString()).findOne({_id: 1}));
mongosConnNew = authDBUsers(new Mongo(mongos.host));
assert.neq(null, mongosConnNew.getCollection(collUnsharded.toString()).findOne({_id: 1}));

mongosConnNew = authDBUsers(new Mongo(mongos.host));
assert.commandWorked(mongosConnNew.getCollection(collSharded.toString()).insert({_id: -4}, wc));
mongosConnNew = authDBUsers(new Mongo(mongos.host));
assert.commandWorked(mongosConnNew.getCollection(collSharded.toString()).insert({_id: 4}, wc));
mongosConnNew = authDBUsers(new Mongo(mongos.host));
assert.commandWorked(mongosConnNew.getCollection(collUnsharded.toString()).insert({_id: 4}, wc));

gc(); // Clean up new connections

jsTest.log("Stopping primary of second shard...");

mongosConnActive.setSecondaryOk();
mongosConnIdle = authDBUsers(new Mongo(mongos.host));
mongosConnIdle.setSecondaryOk();

// Need to save this node for later
let rs1Secondary = st.rs1.getSecondary();

st.rs1.stop(st.rs1.getPrimary());

jsTest.log("Testing active connection with second primary down...");

assert.neq(null, mongosConnActive.getCollection(collSharded.toString()).findOne({_id: -1}));
assert.neq(null, mongosConnActive.getCollection(collSharded.toString()).findOne({_id: 1}));
assert.neq(null, mongosConnActive.getCollection(collUnsharded.toString()).findOne({_id: 1}));

assert.commandWorked(mongosConnActive.getCollection(collSharded.toString()).insert({_id: -5}, wc));
assert.writeError(mongosConnActive.getCollection(collSharded.toString()).insert({_id: 5}, wc));
assert.commandWorked(mongosConnActive.getCollection(collUnsharded.toString()).insert({_id: 5}, wc));

jsTest.log("Testing idle connection with second primary down...");

assert.commandWorked(mongosConnIdle.getCollection(collSharded.toString()).insert({_id: -6}, wc));
assert.writeError(mongosConnIdle.getCollection(collSharded.toString()).insert({_id: 6}, wc));
assert.commandWorked(mongosConnIdle.getCollection(collUnsharded.toString()).insert({_id: 6}, wc));

assert.neq(null, mongosConnIdle.getCollection(collSharded.toString()).findOne({_id: -1}));
assert.neq(null, mongosConnIdle.getCollection(collSharded.toString()).findOne({_id: 1}));
assert.neq(null, mongosConnIdle.getCollection(collUnsharded.toString()).findOne({_id: 1}));

jsTest.log("Testing new connections with second primary down...");

mongosConnNew = authDBUsers(new Mongo(mongos.host));
mongosConnNew.setSecondaryOk();
assert.neq(null, mongosConnNew.getCollection(collSharded.toString()).findOne({_id: -1}));
mongosConnNew = authDBUsers(new Mongo(mongos.host));
mongosConnNew.setSecondaryOk();
assert.neq(null, mongosConnNew.getCollection(collSharded.toString()).findOne({_id: 1}));
mongosConnNew = authDBUsers(new Mongo(mongos.host));
mongosConnNew.setSecondaryOk();
assert.neq(null, mongosConnNew.getCollection(collUnsharded.toString()).findOne({_id: 1}));

mongosConnNew = authDBUsers(new Mongo(mongos.host));
assert.commandWorked(mongosConnNew.getCollection(collSharded.toString()).insert({_id: -7}, wc));
mongosConnNew = authDBUsers(new Mongo(mongos.host));
assert.writeError(mongosConnNew.getCollection(collSharded.toString()).insert({_id: 7}, wc));
mongosConnNew = authDBUsers(new Mongo(mongos.host));
assert.commandWorked(mongosConnNew.getCollection(collUnsharded.toString()).insert({_id: 7}, wc));

gc(); // Clean up new connections

jsTest.log("Stopping primary of first shard...");

mongosConnActive.setSecondaryOk();
mongosConnIdle = authDBUsers(new Mongo(mongos.host));
mongosConnIdle.setSecondaryOk();

st.rs0.stop(st.rs0.getPrimary());

jsTest.log("Testing active connection with first primary down...");

assert.neq(null, mongosConnActive.getCollection(collSharded.toString()).findOne({_id: -1}));
assert.neq(null, mongosConnActive.getCollection(collSharded.toString()).findOne({_id: 1}));
assert.neq(null, mongosConnActive.getCollection(collUnsharded.toString()).findOne({_id: 1}));

assert.writeError(mongosConnActive.getCollection(collSharded.toString()).insert({_id: -8}));
assert.writeError(mongosConnActive.getCollection(collSharded.toString()).insert({_id: 8}));
assert.writeError(mongosConnActive.getCollection(collUnsharded.toString()).insert({_id: 8}));

jsTest.log("Testing idle connection with first primary down...");

assert.writeError(mongosConnIdle.getCollection(collSharded.toString()).insert({_id: -9}));
assert.writeError(mongosConnIdle.getCollection(collSharded.toString()).insert({_id: 9}));
assert.writeError(mongosConnIdle.getCollection(collUnsharded.toString()).insert({_id: 9}));

assert.neq(null, mongosConnIdle.getCollection(collSharded.toString()).findOne({_id: -1}));
assert.neq(null, mongosConnIdle.getCollection(collSharded.toString()).findOne({_id: 1}));
assert.neq(null, mongosConnIdle.getCollection(collUnsharded.toString()).findOne({_id: 1}));

jsTest.log("Testing new connections with first primary down...");

mongosConnNew = authDBUsers(new Mongo(mongos.host));
mongosConnNew.setSecondaryOk();
assert.neq(null, mongosConnNew.getCollection(collSharded.toString()).findOne({_id: -1}));
mongosConnNew = authDBUsers(new Mongo(mongos.host));
mongosConnNew.setSecondaryOk();
assert.neq(null, mongosConnNew.getCollection(collSharded.toString()).findOne({_id: 1}));
mongosConnNew = authDBUsers(new Mongo(mongos.host));
mongosConnNew.setSecondaryOk();
assert.neq(null, mongosConnNew.getCollection(collUnsharded.toString()).findOne({_id: 1}));

mongosConnNew = authDBUsers(new Mongo(mongos.host));
assert.writeError(mongosConnNew.getCollection(collSharded.toString()).insert({_id: -10}));
mongosConnNew = authDBUsers(new Mongo(mongos.host));
assert.writeError(mongosConnNew.getCollection(collSharded.toString()).insert({_id: 10}));
mongosConnNew = authDBUsers(new Mongo(mongos.host));
assert.writeError(mongosConnNew.getCollection(collUnsharded.toString()).insert({_id: 10}));

gc(); // Clean up new connections

jsTest.log("Stopping second shard...");

mongosConnActive.setSecondaryOk();
mongosConnIdle = authDBUsers(new Mongo(mongos.host));
mongosConnIdle.setSecondaryOk();

st.rs1.stop(rs1Secondary);

jsTest.log("Testing active connection with second shard down...");

assert.neq(null, mongosConnActive.getCollection(collSharded.toString()).findOne({_id: -1}));
assert.neq(null, mongosConnActive.getCollection(collUnsharded.toString()).findOne({_id: 1}));

assert.writeError(mongosConnActive.getCollection(collSharded.toString()).insert({_id: -11}));
assert.writeError(mongosConnActive.getCollection(collSharded.toString()).insert({_id: 11}));
assert.writeError(mongosConnActive.getCollection(collUnsharded.toString()).insert({_id: 11}));

jsTest.log("Testing idle connection with second shard down...");

assert.writeError(mongosConnIdle.getCollection(collSharded.toString()).insert({_id: -12}));
assert.writeError(mongosConnIdle.getCollection(collSharded.toString()).insert({_id: 12}));
assert.writeError(mongosConnIdle.getCollection(collUnsharded.toString()).insert({_id: 12}));

assert.neq(null, mongosConnIdle.getCollection(collSharded.toString()).findOne({_id: -1}));
assert.neq(null, mongosConnIdle.getCollection(collUnsharded.toString()).findOne({_id: 1}));

jsTest.log("Testing new connections with second shard down...");

mongosConnNew = authDBUsers(new Mongo(mongos.host));
mongosConnNew.setSecondaryOk();
assert.neq(null, mongosConnNew.getCollection(collSharded.toString()).findOne({_id: -1}));
mongosConnNew = authDBUsers(new Mongo(mongos.host));
mongosConnNew.setSecondaryOk();
assert.neq(null, mongosConnNew.getCollection(collUnsharded.toString()).findOne({_id: 1}));

mongosConnNew = authDBUsers(new Mongo(mongos.host));
assert.writeError(mongosConnNew.getCollection(collSharded.toString()).insert({_id: -13}));
mongosConnNew = authDBUsers(new Mongo(mongos.host));
assert.writeError(mongosConnNew.getCollection(collSharded.toString()).insert({_id: 13}));
mongosConnNew = authDBUsers(new Mongo(mongos.host));
assert.writeError(mongosConnNew.getCollection(collUnsharded.toString()).insert({_id: 13}));

gc(); // Clean up new connections

jsTest.log("DONE!");
st.stop();
