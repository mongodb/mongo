//
// Tests mongos's failure tolerance for authenticated replica set shards and slaveOk queries using
// legacy authentication
//
// TODO: Remove test post-2.6
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

var options = { separateConfig : true,
                rs : true,
                enableBalancer: true,
                mongosOptions : { binVersion : "2.6" },
                rsOptions : { nodes : 2, binVersion : "2.4" },
                keyFile : "jstests/libs/key1" };

var st = new ShardingTest({shards : 3, mongos : 1, other : options});
st.stopBalancer();

var mongos = st.s0;
var admin = mongos.getDB( "admin" );
var shards = mongos.getDB( "config" ).shards.find().toArray();

assert.commandWorked( admin.runCommand({ setParameter : 1, traceExceptions : true }) );

var collSharded = mongos.getCollection( "fooSharded.barSharded" );
var collUnsharded = mongos.getCollection( "fooUnsharded.barUnsharded" );

// Create the unsharded database with shard0 primary
collUnsharded.insert({ some : "doc" });
assert.eq( null, collUnsharded.getDB().getLastError() );
collUnsharded.remove({});
assert.eq( null, collUnsharded.getDB().getLastError() );
printjson( admin.runCommand({ movePrimary : collUnsharded.getDB().toString(),
                              to : shards[0]._id }) );

// Create the sharded database with shard1 primary
assert.commandWorked( admin.runCommand({ enableSharding : collSharded.getDB().toString() }) );
printjson( admin.runCommand({ movePrimary : collSharded.getDB().toString(), to : shards[1]._id }) );
assert.commandWorked( admin.runCommand({ shardCollection : collSharded.toString(),
                                         key : { _id : 1 } }) );
assert.commandWorked( admin.runCommand({ split : collSharded.toString(), middle : { _id : 0 } }) );
assert.commandWorked( admin.runCommand({ moveChunk : collSharded.toString(),
                                         find : { _id : -1 },
                                         to : shards[0]._id }) );

st.printShardingStatus();

var adminUser = "adminUser";
var shardedDBUser = "shardedDBUser";
var unshardedDBUser = "unshardedDBUser";
var password = "password";

jsTest.log("Setting up (legacy) database users...");

// Create db users
collSharded.getDB()._addUserWithInsert({ user : shardedDBUser,
                                         pwd : password, roles : [ "readWrite" ] });
collUnsharded.getDB()._addUserWithInsert({ user : unshardedDBUser,
                                           pwd : password, roles : [ "readWrite" ] });

jsTest.log("Setting up (legacy) admin user...");

// Create a user
admin._addUserWithInsert({ user : adminUser, pwd : password, roles: [ "userAdminAnyDatabase" ] });
// There's an admin user now, so we need to login to do anything

function authDBUsers( conn ) {
    conn.getDB( collSharded.getDB().toString() ).auth(shardedDBUser, password);
    conn.getDB( collUnsharded.getDB().toString() ).auth(unshardedDBUser, password);
    return conn;
}

function authUnshardedUser( conn ) {
    conn.getDB( collUnsharded.getDB().toString() ).auth(unshardedDBUser, password);
    return conn;
}

// Needed b/c the GLE command itself can fail if the shard is down ("write result unknown") - we
// don't care if this happens in this test, we only care that we did not get "write succeeded".
// Depending on the connection pool state, we could get either.
function gleErrorOrThrow(database, msg) {
    var gle;
    try {
        gle = database.getLastErrorObj();
    }
    catch (ex) {
        return;
    }
    if (!gle.err) doassert("getLastError is null: " + tojson(gle) + " :" + msg);
    return;
};

//
// Setup is complete
//

jsTest.log("Inserting initial data...");

var mongosConnActive = authDBUsers( new Mongo( mongos.host ) );
authDBUsers(mongosConnActive);
var mongosConnIdle = null;
var mongosConnNew = null;

mongosConnActive.getCollection( collSharded.toString() ).insert({ _id : -1 });
mongosConnActive.getCollection( collSharded.toString() ).insert({ _id : 1 });
assert.eq(null, mongosConnActive.getCollection( collSharded.toString() ).getDB().getLastError());

mongosConnActive.getCollection( collUnsharded.toString() ).insert({ _id : 1 });
assert.eq(null, mongosConnActive.getCollection( collUnsharded.toString() ).getDB().getLastError());

jsTest.log("Stopping primary of third shard...");

mongosConnIdle = authDBUsers( new Mongo( mongos.host ) );

st.rs2.stop(st.rs2.getPrimary(), true ); // wait for stop

jsTest.log("Testing active connection with third primary down...");

assert.neq(null, mongosConnActive.getCollection( collSharded.toString() ).findOne({ _id : -1 }));
assert.neq(null, mongosConnActive.getCollection( collSharded.toString() ).findOne({ _id : 1 }));
assert.neq(null, mongosConnActive.getCollection( collUnsharded.toString() ).findOne({ _id : 1 }));

mongosConnActive.getCollection( collSharded.toString() ).insert({ _id : -2 });
assert.gleSuccess(mongosConnActive.getCollection( collSharded.toString() ).getDB());
mongosConnActive.getCollection( collSharded.toString() ).insert({ _id : 2 });
assert.gleSuccess(mongosConnActive.getCollection( collSharded.toString() ).getDB());
mongosConnActive.getCollection( collUnsharded.toString() ).insert({ _id : 2 });
assert.gleSuccess(mongosConnActive.getCollection( collUnsharded.toString() ).getDB());

jsTest.log("Testing idle connection with third primary down...");

mongosConnIdle.getCollection( collSharded.toString() ).insert({ _id : -3 });
assert.gleSuccess(mongosConnIdle.getCollection( collSharded.toString() ).getDB());
mongosConnIdle.getCollection( collSharded.toString() ).insert({ _id : 3 });
assert.gleSuccess(mongosConnIdle.getCollection( collSharded.toString() ).getDB());
mongosConnIdle.getCollection( collUnsharded.toString() ).insert({ _id : 3 });
assert.gleSuccess(mongosConnIdle.getCollection( collUnsharded.toString() ).getDB());

assert.neq(null, mongosConnIdle.getCollection( collSharded.toString() ).findOne({ _id : -1 }) );
assert.neq(null, mongosConnIdle.getCollection( collSharded.toString() ).findOne({ _id : 1 }) );
assert.neq(null, mongosConnIdle.getCollection( collUnsharded.toString() ).findOne({ _id : 1 }) );

jsTest.log("Testing new connections with third primary down...");

mongosConnNew = authDBUsers( new Mongo( mongos.host ) );
assert.neq(null, mongosConnNew.getCollection( collSharded.toString() ).findOne({ _id : -1 }) );
mongosConnNew = authDBUsers( new Mongo( mongos.host ) );
assert.neq(null, mongosConnNew.getCollection( collSharded.toString() ).findOne({ _id : 1 }) );
mongosConnNew = authDBUsers( new Mongo( mongos.host ) );
assert.neq(null, mongosConnNew.getCollection( collUnsharded.toString() ).findOne({ _id : 1 }) );

mongosConnNew = authDBUsers( new Mongo( mongos.host ) );
mongosConnNew.getCollection( collSharded.toString() ).insert({ _id : -4 });
assert.gleSuccess(mongosConnNew.getCollection( collSharded.toString() ).getDB());
mongosConnNew = authDBUsers( new Mongo( mongos.host ) );
mongosConnNew.getCollection( collSharded.toString() ).insert({ _id : 4 });
assert.gleSuccess(mongosConnNew.getCollection( collSharded.toString() ).getDB());
mongosConnNew = authDBUsers( new Mongo( mongos.host ) );
mongosConnNew.getCollection( collUnsharded.toString() ).insert({ _id : 4 });
assert.gleSuccess(mongosConnNew.getCollection( collUnsharded.toString() ).getDB());

gc(); // Clean up new connections

jsTest.log("Stopping primary of second shard...");

mongosConnActive.setSlaveOk();
mongosConnIdle = authDBUsers( new Mongo( mongos.host ) );
mongosConnIdle.setSlaveOk();

// Need to save this node for later
var rs1Secondary = st.rs1.getSecondary();

st.rs1.stop(st.rs1.getPrimary(), true ); // wait for stop

jsTest.log("Testing active connection with second primary down...");

assert.neq(null, mongosConnActive.getCollection( collSharded.toString() ).findOne({ _id : -1 }));
assert.neq(null, mongosConnActive.getCollection( collSharded.toString() ).findOne({ _id : 1 }));
assert.neq(null, mongosConnActive.getCollection( collUnsharded.toString() ).findOne({ _id : 1 }));

mongosConnActive.getCollection( collSharded.toString() ).insert({ _id : -5 });
assert.gleSuccess(mongosConnActive.getCollection( collSharded.toString() ).getDB());
mongosConnActive.getCollection( collSharded.toString() ).insert({ _id : 5 });
gleErrorOrThrow(mongosConnActive.getCollection( collSharded.toString() ).getDB());
mongosConnActive.getCollection( collUnsharded.toString() ).insert({ _id : 5 });
assert.gleSuccess(mongosConnActive.getCollection( collUnsharded.toString() ).getDB());

jsTest.log("Testing idle connection with second primary down...");

mongosConnIdle.getCollection( collSharded.toString() ).insert({ _id : -6 });
assert.gleSuccess(mongosConnIdle.getCollection( collSharded.toString() ).getDB());
mongosConnIdle.getCollection( collSharded.toString() ).insert({ _id : 6 });
gleErrorOrThrow(mongosConnIdle.getCollection( collSharded.toString() ).getDB());
mongosConnIdle.getCollection( collUnsharded.toString() ).insert({ _id : 6 });
assert.gleSuccess(mongosConnIdle.getCollection( collUnsharded.toString() ).getDB());

assert.neq(null, mongosConnIdle.getCollection( collSharded.toString() ).findOne({ _id : -1 }) );
assert.neq(null, mongosConnIdle.getCollection( collSharded.toString() ).findOne({ _id : 1 }) );
assert.neq(null, mongosConnIdle.getCollection( collUnsharded.toString() ).findOne({ _id : 1 }) );

jsTest.log("Testing new connections with second primary down...");

mongosConnNew = authDBUsers( new Mongo( mongos.host ) );
mongosConnNew.setSlaveOk();
assert.neq(null, mongosConnNew.getCollection( collSharded.toString() ).findOne({ _id : -1 }) );
mongosConnNew = authDBUsers( new Mongo( mongos.host ) );
mongosConnNew.setSlaveOk();
assert.neq(null, mongosConnNew.getCollection( collSharded.toString() ).findOne({ _id : 1 }) );
mongosConnNew = authDBUsers( new Mongo( mongos.host ) );
mongosConnNew.setSlaveOk();
assert.neq(null, mongosConnNew.getCollection( collUnsharded.toString() ).findOne({ _id : 1 }) );

mongosConnNew = authDBUsers( new Mongo( mongos.host ) );
mongosConnNew.getCollection( collSharded.toString() ).insert({ _id : -7 });
assert.gleSuccess(mongosConnNew.getCollection( collSharded.toString() ).getDB());
mongosConnNew = authDBUsers( new Mongo( mongos.host ) );
mongosConnNew.getCollection( collSharded.toString() ).insert({ _id : 7 });
gleErrorOrThrow(mongosConnNew.getCollection( collSharded.toString() ).getDB());
mongosConnNew = authDBUsers( new Mongo( mongos.host ) );
mongosConnNew.getCollection( collUnsharded.toString() ).insert({ _id : 7 });
assert.gleSuccess(mongosConnNew.getCollection( collUnsharded.toString() ).getDB());

gc(); // Clean up new connections

jsTest.log("Stopping primary of first shard...");

mongosConnActive.setSlaveOk();
mongosConnIdle = authDBUsers( new Mongo( mongos.host ) );
mongosConnIdle.setSlaveOk();

st.rs0.stop(st.rs0.getPrimary(), true ); // wait for stop

jsTest.log("Testing active connection with first primary down...");

assert.neq(null, mongosConnActive.getCollection( collSharded.toString() ).findOne({ _id : -1 }));
assert.neq(null, mongosConnActive.getCollection( collSharded.toString() ).findOne({ _id : 1 }));
assert.neq(null, mongosConnActive.getCollection( collUnsharded.toString() ).findOne({ _id : 1 }));

mongosConnActive.getCollection( collSharded.toString() ).insert({ _id : -8 });
gleErrorOrThrow(mongosConnActive.getCollection( collSharded.toString() ).getDB());
mongosConnActive.getCollection( collSharded.toString() ).insert({ _id : 8 });
gleErrorOrThrow(mongosConnActive.getCollection( collSharded.toString() ).getDB());
mongosConnActive.getCollection( collUnsharded.toString() ).insert({ _id : 8 });
gleErrorOrThrow(mongosConnActive.getCollection( collUnsharded.toString() ).getDB());

jsTest.log("Testing idle connection with first primary down...");

mongosConnIdle.getCollection( collSharded.toString() ).insert({ _id : -9 });
gleErrorOrThrow(mongosConnIdle.getCollection( collSharded.toString() ).getDB());
mongosConnIdle.getCollection( collSharded.toString() ).insert({ _id : 9 });
gleErrorOrThrow(mongosConnIdle.getCollection( collSharded.toString() ).getDB());
mongosConnIdle.getCollection( collUnsharded.toString() ).insert({ _id : 9 });
gleErrorOrThrow(mongosConnIdle.getCollection( collUnsharded.toString() ).getDB());

assert.neq(null, mongosConnIdle.getCollection( collSharded.toString() ).findOne({ _id : -1 }) );
assert.neq(null, mongosConnIdle.getCollection( collSharded.toString() ).findOne({ _id : 1 }) );
assert.neq(null, mongosConnIdle.getCollection( collUnsharded.toString() ).findOne({ _id : 1 }) );

jsTest.log("Testing new connections with first primary down...");

mongosConnNew = authDBUsers( new Mongo( mongos.host ) );
mongosConnNew.setSlaveOk();
assert.neq(null, mongosConnNew.getCollection( collSharded.toString() ).findOne({ _id : -1 }) );
mongosConnNew = authDBUsers( new Mongo( mongos.host ) );
mongosConnNew.setSlaveOk();
assert.neq(null, mongosConnNew.getCollection( collSharded.toString() ).findOne({ _id : 1 }) );
mongosConnNew = authDBUsers( new Mongo( mongos.host ) );
mongosConnNew.setSlaveOk();
assert.neq(null, mongosConnNew.getCollection( collUnsharded.toString() ).findOne({ _id : 1 }) );

mongosConnNew = authDBUsers( new Mongo( mongos.host ) );
mongosConnNew.getCollection( collSharded.toString() ).insert({ _id : -10 });
gleErrorOrThrow(mongosConnNew.getCollection( collSharded.toString() ).getDB());
mongosConnNew = authDBUsers( new Mongo( mongos.host ) );
mongosConnNew.getCollection( collSharded.toString() ).insert({ _id : 10 });
gleErrorOrThrow(mongosConnNew.getCollection( collSharded.toString() ).getDB());
mongosConnNew = authDBUsers( new Mongo( mongos.host ) );
mongosConnNew.getCollection( collUnsharded.toString() ).insert({ _id : 10 });
gleErrorOrThrow(mongosConnNew.getCollection( collUnsharded.toString() ).getDB());

gc(); // Clean up new connections

jsTest.log("Stopping second shard...");

mongosConnActive.setSlaveOk();
mongosConnIdle = authDBUsers( new Mongo( mongos.host ) );
mongosConnIdle.setSlaveOk();

st.rs1.stop(rs1Secondary, true ); // wait for stop

jsTest.log("Testing active connection with second shard down...");

assert.neq(null, mongosConnActive.getCollection( collSharded.toString() ).findOne({ _id : -1 }));
assert.neq(null, mongosConnActive.getCollection( collUnsharded.toString() ).findOne({ _id : 1 }));

mongosConnActive.getCollection( collSharded.toString() ).insert({ _id : -11 });
gleErrorOrThrow(mongosConnActive.getCollection( collSharded.toString() ).getDB());
mongosConnActive.getCollection( collSharded.toString() ).insert({ _id : 11 });
gleErrorOrThrow(mongosConnActive.getCollection( collSharded.toString() ).getDB());
mongosConnActive.getCollection( collUnsharded.toString() ).insert({ _id : 11 });
gleErrorOrThrow(mongosConnActive.getCollection( collUnsharded.toString() ).getDB());

jsTest.log("Testing idle connection with second shard down...");

mongosConnIdle.getCollection( collSharded.toString() ).insert({ _id : -12 });
gleErrorOrThrow(mongosConnIdle.getCollection( collSharded.toString() ).getDB());
mongosConnIdle.getCollection( collSharded.toString() ).insert({ _id : 12 });
gleErrorOrThrow(mongosConnIdle.getCollection( collSharded.toString() ).getDB());
mongosConnIdle.getCollection( collUnsharded.toString() ).insert({ _id : 12 });
gleErrorOrThrow(mongosConnIdle.getCollection( collUnsharded.toString() ).getDB());

assert.neq(null, mongosConnIdle.getCollection( collSharded.toString() ).findOne({ _id : -1 }) );
assert.neq(null, mongosConnIdle.getCollection( collUnsharded.toString() ).findOne({ _id : 1 }) );

jsTest.log("Testing new connections with second shard down...");

// Note that this would fail for the sharded database with a primary on the second shard

mongosConnNew = authUnshardedUser( new Mongo( mongos.host ) );
mongosConnNew.setSlaveOk();
assert.neq(null, mongosConnNew.getCollection( collUnsharded.toString() ).findOne({ _id : 1 }) );

mongosConnNew = authUnshardedUser( new Mongo( mongos.host ) );
mongosConnNew.getCollection( collUnsharded.toString() ).insert({ _id : 13 });
gleErrorOrThrow(mongosConnNew.getCollection( collUnsharded.toString() ).getDB());

gc(); // Clean up new connections

jsTest.log("DONE!");
st.stop();
