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

var options = {rs: true, rsOptions: {nodes: 2}, keyFile: "jstests/libs/key1"};

var st = new ShardingTest({shards: 3, mongos: 1, other: options});

var mongos = st.s0;
var admin = mongos.getDB("admin");

jsTest.log("Setting up initial admin user...");
var adminUser = "adminUser";
var password = "password";

// Create a user
admin.createUser({user: adminUser, pwd: password, roles: ["root"]});
// There's an admin user now, so we need to login to do anything
// Login as admin user
admin.auth(adminUser, password);

st.stopBalancer();
var shards = mongos.getDB("config").shards.find().toArray();

assert.commandWorked(admin.runCommand({setParameter: 1, traceExceptions: true}));

var collSharded = mongos.getCollection("fooSharded.barSharded");
var collUnsharded = mongos.getCollection("fooUnsharded.barUnsharded");

// Create the unsharded database with shard0 primary
assert.writeOK(collUnsharded.insert({some: "doc"}));
assert.writeOK(collUnsharded.remove({}));
printjson(admin.runCommand({movePrimary: collUnsharded.getDB().toString(), to: shards[0]._id}));

// Create the sharded database with shard1 primary
assert.commandWorked(admin.runCommand({enableSharding: collSharded.getDB().toString()}));
printjson(admin.runCommand({movePrimary: collSharded.getDB().toString(), to: shards[1]._id}));
assert.commandWorked(admin.runCommand({shardCollection: collSharded.toString(), key: {_id: 1}}));
assert.commandWorked(admin.runCommand({split: collSharded.toString(), middle: {_id: 0}}));
assert.commandWorked(
    admin.runCommand({moveChunk: collSharded.toString(), find: {_id: -1}, to: shards[0]._id}));

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

//
// Setup is complete
//

jsTest.log("Inserting initial data...");

var mongosConnActive = authDBUsers(new Mongo(mongos.host));
authDBUsers(mongosConnActive);
var mongosConnIdle = null;
var mongosConnNew = null;

var wc = {writeConcern: {w: 2, wtimeout: 60000}};

assert.writeOK(mongosConnActive.getCollection(collSharded.toString()).insert({_id: -1}, wc));
assert.writeOK(mongosConnActive.getCollection(collSharded.toString()).insert({_id: 1}, wc));
assert.writeOK(mongosConnActive.getCollection(collUnsharded.toString()).insert({_id: 1}, wc));

jsTest.log("Stopping primary of third shard...");

mongosConnIdle = authDBUsers(new Mongo(mongos.host));

st.rs2.stop(st.rs2.getPrimary());

jsTest.log("Testing active connection with third primary down...");

assert.neq(null, mongosConnActive.getCollection(collSharded.toString()).findOne({_id: -1}));
assert.neq(null, mongosConnActive.getCollection(collSharded.toString()).findOne({_id: 1}));
assert.neq(null, mongosConnActive.getCollection(collUnsharded.toString()).findOne({_id: 1}));

assert.writeOK(mongosConnActive.getCollection(collSharded.toString()).insert({_id: -2}, wc));
assert.writeOK(mongosConnActive.getCollection(collSharded.toString()).insert({_id: 2}, wc));
assert.writeOK(mongosConnActive.getCollection(collUnsharded.toString()).insert({_id: 2}, wc));

jsTest.log("Testing idle connection with third primary down...");

assert.writeOK(mongosConnIdle.getCollection(collSharded.toString()).insert({_id: -3}, wc));
assert.writeOK(mongosConnIdle.getCollection(collSharded.toString()).insert({_id: 3}, wc));
assert.writeOK(mongosConnIdle.getCollection(collUnsharded.toString()).insert({_id: 3}, wc));

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
assert.writeOK(mongosConnNew.getCollection(collSharded.toString()).insert({_id: -4}, wc));
mongosConnNew = authDBUsers(new Mongo(mongos.host));
assert.writeOK(mongosConnNew.getCollection(collSharded.toString()).insert({_id: 4}, wc));
mongosConnNew = authDBUsers(new Mongo(mongos.host));
assert.writeOK(mongosConnNew.getCollection(collUnsharded.toString()).insert({_id: 4}, wc));

gc();  // Clean up new connections

jsTest.log("Stopping primary of second shard...");

mongosConnActive.setSlaveOk();
mongosConnIdle = authDBUsers(new Mongo(mongos.host));
mongosConnIdle.setSlaveOk();

// Need to save this node for later
var rs1Secondary = st.rs1.getSecondary();

st.rs1.stop(st.rs1.getPrimary());

jsTest.log("Testing active connection with second primary down...");

assert.neq(null, mongosConnActive.getCollection(collSharded.toString()).findOne({_id: -1}));
assert.neq(null, mongosConnActive.getCollection(collSharded.toString()).findOne({_id: 1}));
assert.neq(null, mongosConnActive.getCollection(collUnsharded.toString()).findOne({_id: 1}));

assert.writeOK(mongosConnActive.getCollection(collSharded.toString()).insert({_id: -5}, wc));
assert.writeError(mongosConnActive.getCollection(collSharded.toString()).insert({_id: 5}, wc));
assert.writeOK(mongosConnActive.getCollection(collUnsharded.toString()).insert({_id: 5}, wc));

jsTest.log("Testing idle connection with second primary down...");

assert.writeOK(mongosConnIdle.getCollection(collSharded.toString()).insert({_id: -6}, wc));
assert.writeError(mongosConnIdle.getCollection(collSharded.toString()).insert({_id: 6}, wc));
assert.writeOK(mongosConnIdle.getCollection(collUnsharded.toString()).insert({_id: 6}, wc));

assert.neq(null, mongosConnIdle.getCollection(collSharded.toString()).findOne({_id: -1}));
assert.neq(null, mongosConnIdle.getCollection(collSharded.toString()).findOne({_id: 1}));
assert.neq(null, mongosConnIdle.getCollection(collUnsharded.toString()).findOne({_id: 1}));

jsTest.log("Testing new connections with second primary down...");

mongosConnNew = authDBUsers(new Mongo(mongos.host));
mongosConnNew.setSlaveOk();
assert.neq(null, mongosConnNew.getCollection(collSharded.toString()).findOne({_id: -1}));
mongosConnNew = authDBUsers(new Mongo(mongos.host));
mongosConnNew.setSlaveOk();
assert.neq(null, mongosConnNew.getCollection(collSharded.toString()).findOne({_id: 1}));
mongosConnNew = authDBUsers(new Mongo(mongos.host));
mongosConnNew.setSlaveOk();
assert.neq(null, mongosConnNew.getCollection(collUnsharded.toString()).findOne({_id: 1}));

mongosConnNew = authDBUsers(new Mongo(mongos.host));
assert.writeOK(mongosConnNew.getCollection(collSharded.toString()).insert({_id: -7}, wc));
mongosConnNew = authDBUsers(new Mongo(mongos.host));
assert.writeError(mongosConnNew.getCollection(collSharded.toString()).insert({_id: 7}, wc));
mongosConnNew = authDBUsers(new Mongo(mongos.host));
assert.writeOK(mongosConnNew.getCollection(collUnsharded.toString()).insert({_id: 7}, wc));

gc();  // Clean up new connections

jsTest.log("Stopping primary of first shard...");

mongosConnActive.setSlaveOk();
mongosConnIdle = authDBUsers(new Mongo(mongos.host));
mongosConnIdle.setSlaveOk();

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
mongosConnNew.setSlaveOk();
assert.neq(null, mongosConnNew.getCollection(collSharded.toString()).findOne({_id: -1}));
mongosConnNew = authDBUsers(new Mongo(mongos.host));
mongosConnNew.setSlaveOk();
assert.neq(null, mongosConnNew.getCollection(collSharded.toString()).findOne({_id: 1}));
mongosConnNew = authDBUsers(new Mongo(mongos.host));
mongosConnNew.setSlaveOk();
assert.neq(null, mongosConnNew.getCollection(collUnsharded.toString()).findOne({_id: 1}));

mongosConnNew = authDBUsers(new Mongo(mongos.host));
assert.writeError(mongosConnNew.getCollection(collSharded.toString()).insert({_id: -10}));
mongosConnNew = authDBUsers(new Mongo(mongos.host));
assert.writeError(mongosConnNew.getCollection(collSharded.toString()).insert({_id: 10}));
mongosConnNew = authDBUsers(new Mongo(mongos.host));
assert.writeError(mongosConnNew.getCollection(collUnsharded.toString()).insert({_id: 10}));

gc();  // Clean up new connections

jsTest.log("Stopping second shard...");

mongosConnActive.setSlaveOk();
mongosConnIdle = authDBUsers(new Mongo(mongos.host));
mongosConnIdle.setSlaveOk();

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
mongosConnNew.setSlaveOk();
assert.neq(null, mongosConnNew.getCollection(collSharded.toString()).findOne({_id: -1}));
mongosConnNew = authDBUsers(new Mongo(mongos.host));
mongosConnNew.setSlaveOk();
assert.neq(null, mongosConnNew.getCollection(collUnsharded.toString()).findOne({_id: 1}));

mongosConnNew = authDBUsers(new Mongo(mongos.host));
assert.writeError(mongosConnNew.getCollection(collSharded.toString()).insert({_id: -13}));
mongosConnNew = authDBUsers(new Mongo(mongos.host));
assert.writeError(mongosConnNew.getCollection(collSharded.toString()).insert({_id: 13}));
mongosConnNew = authDBUsers(new Mongo(mongos.host));
assert.writeError(mongosConnNew.getCollection(collUnsharded.toString()).insert({_id: 13}));

gc();  // Clean up new connections

jsTest.log("DONE!");
st.stop();
