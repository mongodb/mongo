//
// Tests merizos's failure tolerance for authenticated replica set shards and slaveOk queries
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

// Checking UUID consistency involves talking to shard primaries, but by the end of this test, one
// shard does not have a primary.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

// Replica set nodes started with --shardsvr do not enable key generation until they are added to a
// sharded cluster and reject commands with gossiped clusterTime from users without the
// advanceClusterTime privilege. This causes ShardingTest setup to fail because the shell briefly
// authenticates as __system and recieves clusterTime metadata then will fail trying to gossip that
// time later in setup.
//
// TODO SERVER-32672: remove this flag.
TestData.skipGossipingClusterTime = true;

// TODO SERVER-35447: Multiple users cannot be authenticated on one connection within a session.
TestData.disableImplicitSessions = true;

var options = {rs: true, rsOptions: {nodes: 2}, keyFile: "jstests/libs/key1"};

var st = new ShardingTest({shards: 3, merizos: 1, other: options});

var merizos = st.s0;
var admin = merizos.getDB("admin");

jsTest.log("Setting up initial admin user...");
var adminUser = "adminUser";
var password = "password";

// Create a user
admin.createUser({user: adminUser, pwd: password, roles: ["root"]});
// There's an admin user now, so we need to login to do anything
// Login as admin user
admin.auth(adminUser, password);

st.stopBalancer();

assert.commandWorked(admin.runCommand({setParameter: 1, traceExceptions: true}));

var collSharded = merizos.getCollection("fooSharded.barSharded");
var collUnsharded = merizos.getCollection("fooUnsharded.barUnsharded");

// Create the unsharded database with shard0 primary
assert.writeOK(collUnsharded.insert({some: "doc"}));
assert.writeOK(collUnsharded.remove({}));
assert.commandWorked(
    admin.runCommand({movePrimary: collUnsharded.getDB().toString(), to: st.shard0.shardName}));

// Create the sharded database with shard1 primary
assert.commandWorked(admin.runCommand({enableSharding: collSharded.getDB().toString()}));
assert.commandWorked(
    admin.runCommand({movePrimary: collSharded.getDB().toString(), to: st.shard1.shardName}));
assert.commandWorked(admin.runCommand({shardCollection: collSharded.toString(), key: {_id: 1}}));
assert.commandWorked(admin.runCommand({split: collSharded.toString(), middle: {_id: 0}}));
assert.commandWorked(admin.runCommand(
    {moveChunk: collSharded.toString(), find: {_id: -1}, to: st.shard0.shardName}));

st.printShardingStatus();
var shardedDBUser = "shardedDBUser";
var unshardedDBUser = "unshardedDBUser";

jsTest.log("Setting up database users...");

// Create db users
collSharded.getDB().createUser({user: shardedDBUser, pwd: password, roles: ["readWrite"]});
collUnsharded.getDB().createUser({user: unshardedDBUser, pwd: password, roles: ["readWrite"]});

admin.logout();

function authDBUsers(conn) {
    conn.getDB(collSharded.getDB().toString()).auth(shardedDBUser, password);
    conn.getDB(collUnsharded.getDB().toString()).auth(unshardedDBUser, password);
    return conn;
}

// Secondaries do not refresh their in-memory routing table until a request with a higher version
// is received, and refreshing requires communication with the primary to obtain the newest version.
// Read from the secondaries once before taking down primaries to ensure they have loaded the
// routing table into memory.
// TODO SERVER-30148: replace this with calls to awaitReplication() on each shard owning data for
// the sharded collection once secondaries refresh proactively.
var merizosSetupConn = new Mongo(merizos.host);
authDBUsers(merizosSetupConn);
merizosSetupConn.setReadPref("secondary");
assert(!merizosSetupConn.getCollection(collSharded.toString()).find({}).hasNext());

gc();  // Clean up connections

//
// Setup is complete
//

jsTest.log("Inserting initial data...");

var merizosConnActive = authDBUsers(new Mongo(merizos.host));
authDBUsers(merizosConnActive);
var merizosConnIdle = null;
var merizosConnNew = null;

var wc = {writeConcern: {w: 2, wtimeout: 60000}};

assert.writeOK(merizosConnActive.getCollection(collSharded.toString()).insert({_id: -1}, wc));
assert.writeOK(merizosConnActive.getCollection(collSharded.toString()).insert({_id: 1}, wc));
assert.writeOK(merizosConnActive.getCollection(collUnsharded.toString()).insert({_id: 1}, wc));

jsTest.log("Stopping primary of third shard...");

merizosConnIdle = authDBUsers(new Mongo(merizos.host));

st.rs2.stop(st.rs2.getPrimary());

jsTest.log("Testing active connection with third primary down...");

assert.neq(null, merizosConnActive.getCollection(collSharded.toString()).findOne({_id: -1}));
assert.neq(null, merizosConnActive.getCollection(collSharded.toString()).findOne({_id: 1}));
assert.neq(null, merizosConnActive.getCollection(collUnsharded.toString()).findOne({_id: 1}));

assert.writeOK(merizosConnActive.getCollection(collSharded.toString()).insert({_id: -2}, wc));
assert.writeOK(merizosConnActive.getCollection(collSharded.toString()).insert({_id: 2}, wc));
assert.writeOK(merizosConnActive.getCollection(collUnsharded.toString()).insert({_id: 2}, wc));

jsTest.log("Testing idle connection with third primary down...");

assert.writeOK(merizosConnIdle.getCollection(collSharded.toString()).insert({_id: -3}, wc));
assert.writeOK(merizosConnIdle.getCollection(collSharded.toString()).insert({_id: 3}, wc));
assert.writeOK(merizosConnIdle.getCollection(collUnsharded.toString()).insert({_id: 3}, wc));

assert.neq(null, merizosConnIdle.getCollection(collSharded.toString()).findOne({_id: -1}));
assert.neq(null, merizosConnIdle.getCollection(collSharded.toString()).findOne({_id: 1}));
assert.neq(null, merizosConnIdle.getCollection(collUnsharded.toString()).findOne({_id: 1}));

jsTest.log("Testing new connections with third primary down...");

merizosConnNew = authDBUsers(new Mongo(merizos.host));
assert.neq(null, merizosConnNew.getCollection(collSharded.toString()).findOne({_id: -1}));
merizosConnNew = authDBUsers(new Mongo(merizos.host));
assert.neq(null, merizosConnNew.getCollection(collSharded.toString()).findOne({_id: 1}));
merizosConnNew = authDBUsers(new Mongo(merizos.host));
assert.neq(null, merizosConnNew.getCollection(collUnsharded.toString()).findOne({_id: 1}));

merizosConnNew = authDBUsers(new Mongo(merizos.host));
assert.writeOK(merizosConnNew.getCollection(collSharded.toString()).insert({_id: -4}, wc));
merizosConnNew = authDBUsers(new Mongo(merizos.host));
assert.writeOK(merizosConnNew.getCollection(collSharded.toString()).insert({_id: 4}, wc));
merizosConnNew = authDBUsers(new Mongo(merizos.host));
assert.writeOK(merizosConnNew.getCollection(collUnsharded.toString()).insert({_id: 4}, wc));

gc();  // Clean up new connections

jsTest.log("Stopping primary of second shard...");

merizosConnActive.setSlaveOk();
merizosConnIdle = authDBUsers(new Mongo(merizos.host));
merizosConnIdle.setSlaveOk();

// Need to save this node for later
var rs1Secondary = st.rs1.getSecondary();

st.rs1.stop(st.rs1.getPrimary());

jsTest.log("Testing active connection with second primary down...");

assert.neq(null, merizosConnActive.getCollection(collSharded.toString()).findOne({_id: -1}));
assert.neq(null, merizosConnActive.getCollection(collSharded.toString()).findOne({_id: 1}));
assert.neq(null, merizosConnActive.getCollection(collUnsharded.toString()).findOne({_id: 1}));

assert.writeOK(merizosConnActive.getCollection(collSharded.toString()).insert({_id: -5}, wc));
assert.writeError(merizosConnActive.getCollection(collSharded.toString()).insert({_id: 5}, wc));
assert.writeOK(merizosConnActive.getCollection(collUnsharded.toString()).insert({_id: 5}, wc));

jsTest.log("Testing idle connection with second primary down...");

assert.writeOK(merizosConnIdle.getCollection(collSharded.toString()).insert({_id: -6}, wc));
assert.writeError(merizosConnIdle.getCollection(collSharded.toString()).insert({_id: 6}, wc));
assert.writeOK(merizosConnIdle.getCollection(collUnsharded.toString()).insert({_id: 6}, wc));

assert.neq(null, merizosConnIdle.getCollection(collSharded.toString()).findOne({_id: -1}));
assert.neq(null, merizosConnIdle.getCollection(collSharded.toString()).findOne({_id: 1}));
assert.neq(null, merizosConnIdle.getCollection(collUnsharded.toString()).findOne({_id: 1}));

jsTest.log("Testing new connections with second primary down...");

merizosConnNew = authDBUsers(new Mongo(merizos.host));
merizosConnNew.setSlaveOk();
assert.neq(null, merizosConnNew.getCollection(collSharded.toString()).findOne({_id: -1}));
merizosConnNew = authDBUsers(new Mongo(merizos.host));
merizosConnNew.setSlaveOk();
assert.neq(null, merizosConnNew.getCollection(collSharded.toString()).findOne({_id: 1}));
merizosConnNew = authDBUsers(new Mongo(merizos.host));
merizosConnNew.setSlaveOk();
assert.neq(null, merizosConnNew.getCollection(collUnsharded.toString()).findOne({_id: 1}));

merizosConnNew = authDBUsers(new Mongo(merizos.host));
assert.writeOK(merizosConnNew.getCollection(collSharded.toString()).insert({_id: -7}, wc));
merizosConnNew = authDBUsers(new Mongo(merizos.host));
assert.writeError(merizosConnNew.getCollection(collSharded.toString()).insert({_id: 7}, wc));
merizosConnNew = authDBUsers(new Mongo(merizos.host));
assert.writeOK(merizosConnNew.getCollection(collUnsharded.toString()).insert({_id: 7}, wc));

gc();  // Clean up new connections

jsTest.log("Stopping primary of first shard...");

merizosConnActive.setSlaveOk();
merizosConnIdle = authDBUsers(new Mongo(merizos.host));
merizosConnIdle.setSlaveOk();

st.rs0.stop(st.rs0.getPrimary());

jsTest.log("Testing active connection with first primary down...");

assert.neq(null, merizosConnActive.getCollection(collSharded.toString()).findOne({_id: -1}));
assert.neq(null, merizosConnActive.getCollection(collSharded.toString()).findOne({_id: 1}));
assert.neq(null, merizosConnActive.getCollection(collUnsharded.toString()).findOne({_id: 1}));

assert.writeError(merizosConnActive.getCollection(collSharded.toString()).insert({_id: -8}));
assert.writeError(merizosConnActive.getCollection(collSharded.toString()).insert({_id: 8}));
assert.writeError(merizosConnActive.getCollection(collUnsharded.toString()).insert({_id: 8}));

jsTest.log("Testing idle connection with first primary down...");

assert.writeError(merizosConnIdle.getCollection(collSharded.toString()).insert({_id: -9}));
assert.writeError(merizosConnIdle.getCollection(collSharded.toString()).insert({_id: 9}));
assert.writeError(merizosConnIdle.getCollection(collUnsharded.toString()).insert({_id: 9}));

assert.neq(null, merizosConnIdle.getCollection(collSharded.toString()).findOne({_id: -1}));
assert.neq(null, merizosConnIdle.getCollection(collSharded.toString()).findOne({_id: 1}));
assert.neq(null, merizosConnIdle.getCollection(collUnsharded.toString()).findOne({_id: 1}));

jsTest.log("Testing new connections with first primary down...");

merizosConnNew = authDBUsers(new Mongo(merizos.host));
merizosConnNew.setSlaveOk();
assert.neq(null, merizosConnNew.getCollection(collSharded.toString()).findOne({_id: -1}));
merizosConnNew = authDBUsers(new Mongo(merizos.host));
merizosConnNew.setSlaveOk();
assert.neq(null, merizosConnNew.getCollection(collSharded.toString()).findOne({_id: 1}));
merizosConnNew = authDBUsers(new Mongo(merizos.host));
merizosConnNew.setSlaveOk();
assert.neq(null, merizosConnNew.getCollection(collUnsharded.toString()).findOne({_id: 1}));

merizosConnNew = authDBUsers(new Mongo(merizos.host));
assert.writeError(merizosConnNew.getCollection(collSharded.toString()).insert({_id: -10}));
merizosConnNew = authDBUsers(new Mongo(merizos.host));
assert.writeError(merizosConnNew.getCollection(collSharded.toString()).insert({_id: 10}));
merizosConnNew = authDBUsers(new Mongo(merizos.host));
assert.writeError(merizosConnNew.getCollection(collUnsharded.toString()).insert({_id: 10}));

gc();  // Clean up new connections

jsTest.log("Stopping second shard...");

merizosConnActive.setSlaveOk();
merizosConnIdle = authDBUsers(new Mongo(merizos.host));
merizosConnIdle.setSlaveOk();

st.rs1.stop(rs1Secondary);

jsTest.log("Testing active connection with second shard down...");

assert.neq(null, merizosConnActive.getCollection(collSharded.toString()).findOne({_id: -1}));
assert.neq(null, merizosConnActive.getCollection(collUnsharded.toString()).findOne({_id: 1}));

assert.writeError(merizosConnActive.getCollection(collSharded.toString()).insert({_id: -11}));
assert.writeError(merizosConnActive.getCollection(collSharded.toString()).insert({_id: 11}));
assert.writeError(merizosConnActive.getCollection(collUnsharded.toString()).insert({_id: 11}));

jsTest.log("Testing idle connection with second shard down...");

assert.writeError(merizosConnIdle.getCollection(collSharded.toString()).insert({_id: -12}));
assert.writeError(merizosConnIdle.getCollection(collSharded.toString()).insert({_id: 12}));
assert.writeError(merizosConnIdle.getCollection(collUnsharded.toString()).insert({_id: 12}));

assert.neq(null, merizosConnIdle.getCollection(collSharded.toString()).findOne({_id: -1}));
assert.neq(null, merizosConnIdle.getCollection(collUnsharded.toString()).findOne({_id: 1}));

jsTest.log("Testing new connections with second shard down...");

merizosConnNew = authDBUsers(new Mongo(merizos.host));
merizosConnNew.setSlaveOk();
assert.neq(null, merizosConnNew.getCollection(collSharded.toString()).findOne({_id: -1}));
merizosConnNew = authDBUsers(new Mongo(merizos.host));
merizosConnNew.setSlaveOk();
assert.neq(null, merizosConnNew.getCollection(collUnsharded.toString()).findOne({_id: 1}));

merizosConnNew = authDBUsers(new Mongo(merizos.host));
assert.writeError(merizosConnNew.getCollection(collSharded.toString()).insert({_id: -13}));
merizosConnNew = authDBUsers(new Mongo(merizos.host));
assert.writeError(merizosConnNew.getCollection(collSharded.toString()).insert({_id: 13}));
merizosConnNew = authDBUsers(new Mongo(merizos.host));
assert.writeError(merizosConnNew.getCollection(collUnsharded.toString()).insert({_id: 13}));

gc();  // Clean up new connections

jsTest.log("DONE!");
st.stop();
