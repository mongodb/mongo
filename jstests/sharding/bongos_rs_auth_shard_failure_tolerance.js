//
// Tests bongos's failure tolerance for authenticated replica set shards and slaveOk queries
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

var st = new ShardingTest({shards: 3, bongos: 1, other: options});

var bongos = st.s0;
var admin = bongos.getDB("admin");

jsTest.log("Setting up initial admin user...");
var adminUser = "adminUser";
var password = "password";

// Create a user
admin.createUser({user: adminUser, pwd: password, roles: ["root"]});
// There's an admin user now, so we need to login to do anything
// Login as admin user
admin.auth(adminUser, password);

st.stopBalancer();
var shards = bongos.getDB("config").shards.find().toArray();

assert.commandWorked(admin.runCommand({setParameter: 1, traceExceptions: true}));

var collSharded = bongos.getCollection("fooSharded.barSharded");
var collUnsharded = bongos.getCollection("fooUnsharded.barUnsharded");

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

var bongosConnActive = authDBUsers(new Bongo(bongos.host));
authDBUsers(bongosConnActive);
var bongosConnIdle = null;
var bongosConnNew = null;

var wc = {writeConcern: {w: 2, wtimeout: 60000}};

assert.writeOK(bongosConnActive.getCollection(collSharded.toString()).insert({_id: -1}, wc));
assert.writeOK(bongosConnActive.getCollection(collSharded.toString()).insert({_id: 1}, wc));
assert.writeOK(bongosConnActive.getCollection(collUnsharded.toString()).insert({_id: 1}, wc));

jsTest.log("Stopping primary of third shard...");

bongosConnIdle = authDBUsers(new Bongo(bongos.host));

st.rs2.stop(st.rs2.getPrimary());

jsTest.log("Testing active connection with third primary down...");

assert.neq(null, bongosConnActive.getCollection(collSharded.toString()).findOne({_id: -1}));
assert.neq(null, bongosConnActive.getCollection(collSharded.toString()).findOne({_id: 1}));
assert.neq(null, bongosConnActive.getCollection(collUnsharded.toString()).findOne({_id: 1}));

assert.writeOK(bongosConnActive.getCollection(collSharded.toString()).insert({_id: -2}, wc));
assert.writeOK(bongosConnActive.getCollection(collSharded.toString()).insert({_id: 2}, wc));
assert.writeOK(bongosConnActive.getCollection(collUnsharded.toString()).insert({_id: 2}, wc));

jsTest.log("Testing idle connection with third primary down...");

assert.writeOK(bongosConnIdle.getCollection(collSharded.toString()).insert({_id: -3}, wc));
assert.writeOK(bongosConnIdle.getCollection(collSharded.toString()).insert({_id: 3}, wc));
assert.writeOK(bongosConnIdle.getCollection(collUnsharded.toString()).insert({_id: 3}, wc));

assert.neq(null, bongosConnIdle.getCollection(collSharded.toString()).findOne({_id: -1}));
assert.neq(null, bongosConnIdle.getCollection(collSharded.toString()).findOne({_id: 1}));
assert.neq(null, bongosConnIdle.getCollection(collUnsharded.toString()).findOne({_id: 1}));

jsTest.log("Testing new connections with third primary down...");

bongosConnNew = authDBUsers(new Bongo(bongos.host));
assert.neq(null, bongosConnNew.getCollection(collSharded.toString()).findOne({_id: -1}));
bongosConnNew = authDBUsers(new Bongo(bongos.host));
assert.neq(null, bongosConnNew.getCollection(collSharded.toString()).findOne({_id: 1}));
bongosConnNew = authDBUsers(new Bongo(bongos.host));
assert.neq(null, bongosConnNew.getCollection(collUnsharded.toString()).findOne({_id: 1}));

bongosConnNew = authDBUsers(new Bongo(bongos.host));
assert.writeOK(bongosConnNew.getCollection(collSharded.toString()).insert({_id: -4}, wc));
bongosConnNew = authDBUsers(new Bongo(bongos.host));
assert.writeOK(bongosConnNew.getCollection(collSharded.toString()).insert({_id: 4}, wc));
bongosConnNew = authDBUsers(new Bongo(bongos.host));
assert.writeOK(bongosConnNew.getCollection(collUnsharded.toString()).insert({_id: 4}, wc));

gc();  // Clean up new connections

jsTest.log("Stopping primary of second shard...");

bongosConnActive.setSlaveOk();
bongosConnIdle = authDBUsers(new Bongo(bongos.host));
bongosConnIdle.setSlaveOk();

// Need to save this node for later
var rs1Secondary = st.rs1.getSecondary();

st.rs1.stop(st.rs1.getPrimary());

jsTest.log("Testing active connection with second primary down...");

assert.neq(null, bongosConnActive.getCollection(collSharded.toString()).findOne({_id: -1}));
assert.neq(null, bongosConnActive.getCollection(collSharded.toString()).findOne({_id: 1}));
assert.neq(null, bongosConnActive.getCollection(collUnsharded.toString()).findOne({_id: 1}));

assert.writeOK(bongosConnActive.getCollection(collSharded.toString()).insert({_id: -5}, wc));
assert.writeError(bongosConnActive.getCollection(collSharded.toString()).insert({_id: 5}, wc));
assert.writeOK(bongosConnActive.getCollection(collUnsharded.toString()).insert({_id: 5}, wc));

jsTest.log("Testing idle connection with second primary down...");

assert.writeOK(bongosConnIdle.getCollection(collSharded.toString()).insert({_id: -6}, wc));
assert.writeError(bongosConnIdle.getCollection(collSharded.toString()).insert({_id: 6}, wc));
assert.writeOK(bongosConnIdle.getCollection(collUnsharded.toString()).insert({_id: 6}, wc));

assert.neq(null, bongosConnIdle.getCollection(collSharded.toString()).findOne({_id: -1}));
assert.neq(null, bongosConnIdle.getCollection(collSharded.toString()).findOne({_id: 1}));
assert.neq(null, bongosConnIdle.getCollection(collUnsharded.toString()).findOne({_id: 1}));

jsTest.log("Testing new connections with second primary down...");

bongosConnNew = authDBUsers(new Bongo(bongos.host));
bongosConnNew.setSlaveOk();
assert.neq(null, bongosConnNew.getCollection(collSharded.toString()).findOne({_id: -1}));
bongosConnNew = authDBUsers(new Bongo(bongos.host));
bongosConnNew.setSlaveOk();
assert.neq(null, bongosConnNew.getCollection(collSharded.toString()).findOne({_id: 1}));
bongosConnNew = authDBUsers(new Bongo(bongos.host));
bongosConnNew.setSlaveOk();
assert.neq(null, bongosConnNew.getCollection(collUnsharded.toString()).findOne({_id: 1}));

bongosConnNew = authDBUsers(new Bongo(bongos.host));
assert.writeOK(bongosConnNew.getCollection(collSharded.toString()).insert({_id: -7}, wc));
bongosConnNew = authDBUsers(new Bongo(bongos.host));
assert.writeError(bongosConnNew.getCollection(collSharded.toString()).insert({_id: 7}, wc));
bongosConnNew = authDBUsers(new Bongo(bongos.host));
assert.writeOK(bongosConnNew.getCollection(collUnsharded.toString()).insert({_id: 7}, wc));

gc();  // Clean up new connections

jsTest.log("Stopping primary of first shard...");

bongosConnActive.setSlaveOk();
bongosConnIdle = authDBUsers(new Bongo(bongos.host));
bongosConnIdle.setSlaveOk();

st.rs0.stop(st.rs0.getPrimary());

jsTest.log("Testing active connection with first primary down...");

assert.neq(null, bongosConnActive.getCollection(collSharded.toString()).findOne({_id: -1}));
assert.neq(null, bongosConnActive.getCollection(collSharded.toString()).findOne({_id: 1}));
assert.neq(null, bongosConnActive.getCollection(collUnsharded.toString()).findOne({_id: 1}));

assert.writeError(bongosConnActive.getCollection(collSharded.toString()).insert({_id: -8}));
assert.writeError(bongosConnActive.getCollection(collSharded.toString()).insert({_id: 8}));
assert.writeError(bongosConnActive.getCollection(collUnsharded.toString()).insert({_id: 8}));

jsTest.log("Testing idle connection with first primary down...");

assert.writeError(bongosConnIdle.getCollection(collSharded.toString()).insert({_id: -9}));
assert.writeError(bongosConnIdle.getCollection(collSharded.toString()).insert({_id: 9}));
assert.writeError(bongosConnIdle.getCollection(collUnsharded.toString()).insert({_id: 9}));

assert.neq(null, bongosConnIdle.getCollection(collSharded.toString()).findOne({_id: -1}));
assert.neq(null, bongosConnIdle.getCollection(collSharded.toString()).findOne({_id: 1}));
assert.neq(null, bongosConnIdle.getCollection(collUnsharded.toString()).findOne({_id: 1}));

jsTest.log("Testing new connections with first primary down...");

bongosConnNew = authDBUsers(new Bongo(bongos.host));
bongosConnNew.setSlaveOk();
assert.neq(null, bongosConnNew.getCollection(collSharded.toString()).findOne({_id: -1}));
bongosConnNew = authDBUsers(new Bongo(bongos.host));
bongosConnNew.setSlaveOk();
assert.neq(null, bongosConnNew.getCollection(collSharded.toString()).findOne({_id: 1}));
bongosConnNew = authDBUsers(new Bongo(bongos.host));
bongosConnNew.setSlaveOk();
assert.neq(null, bongosConnNew.getCollection(collUnsharded.toString()).findOne({_id: 1}));

bongosConnNew = authDBUsers(new Bongo(bongos.host));
assert.writeError(bongosConnNew.getCollection(collSharded.toString()).insert({_id: -10}));
bongosConnNew = authDBUsers(new Bongo(bongos.host));
assert.writeError(bongosConnNew.getCollection(collSharded.toString()).insert({_id: 10}));
bongosConnNew = authDBUsers(new Bongo(bongos.host));
assert.writeError(bongosConnNew.getCollection(collUnsharded.toString()).insert({_id: 10}));

gc();  // Clean up new connections

jsTest.log("Stopping second shard...");

bongosConnActive.setSlaveOk();
bongosConnIdle = authDBUsers(new Bongo(bongos.host));
bongosConnIdle.setSlaveOk();

st.rs1.stop(rs1Secondary);

jsTest.log("Testing active connection with second shard down...");

assert.neq(null, bongosConnActive.getCollection(collSharded.toString()).findOne({_id: -1}));
assert.neq(null, bongosConnActive.getCollection(collUnsharded.toString()).findOne({_id: 1}));

assert.writeError(bongosConnActive.getCollection(collSharded.toString()).insert({_id: -11}));
assert.writeError(bongosConnActive.getCollection(collSharded.toString()).insert({_id: 11}));
assert.writeError(bongosConnActive.getCollection(collUnsharded.toString()).insert({_id: 11}));

jsTest.log("Testing idle connection with second shard down...");

assert.writeError(bongosConnIdle.getCollection(collSharded.toString()).insert({_id: -12}));
assert.writeError(bongosConnIdle.getCollection(collSharded.toString()).insert({_id: 12}));
assert.writeError(bongosConnIdle.getCollection(collUnsharded.toString()).insert({_id: 12}));

assert.neq(null, bongosConnIdle.getCollection(collSharded.toString()).findOne({_id: -1}));
assert.neq(null, bongosConnIdle.getCollection(collUnsharded.toString()).findOne({_id: 1}));

jsTest.log("Testing new connections with second shard down...");

bongosConnNew = authDBUsers(new Bongo(bongos.host));
bongosConnNew.setSlaveOk();
assert.neq(null, bongosConnNew.getCollection(collSharded.toString()).findOne({_id: -1}));
bongosConnNew = authDBUsers(new Bongo(bongos.host));
bongosConnNew.setSlaveOk();
assert.neq(null, bongosConnNew.getCollection(collUnsharded.toString()).findOne({_id: 1}));

bongosConnNew = authDBUsers(new Bongo(bongos.host));
assert.writeError(bongosConnNew.getCollection(collSharded.toString()).insert({_id: -13}));
bongosConnNew = authDBUsers(new Bongo(bongos.host));
assert.writeError(bongosConnNew.getCollection(collSharded.toString()).insert({_id: 13}));
bongosConnNew = authDBUsers(new Bongo(bongos.host));
assert.writeError(bongosConnNew.getCollection(collUnsharded.toString()).insert({_id: 13}));

gc();  // Clean up new connections

jsTest.log("DONE!");
st.stop();
