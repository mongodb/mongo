//
// Tests mongos's failure tolerance for replica set shards and slaveOk queries
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

var options = {separateConfig : true,
               rs : true,
               rsOptions : { nodes : 2 }};

var st = new ShardingTest({shards : 3, mongos : 1, other : options});
st.stopBalancer();

var mongos = st.s0;
var admin = mongos.getDB( "admin" );
var shards = mongos.getDB( "config" ).shards.find().toArray();

assert.commandWorked( admin.runCommand({ setParameter : 1, traceExceptions : true }) );

var collSharded = mongos.getCollection( "fooSharded.barSharded" );
var collUnsharded = mongos.getCollection( "fooUnsharded.barUnsharded" );

assert.commandWorked( admin.runCommand({ enableSharding : collSharded.getDB() + "" }) );
printjson( admin.runCommand({ movePrimary : collSharded.getDB() + "", to : shards[0]._id }) );
assert.commandWorked( admin.runCommand({ shardCollection : collSharded + "", key : { _id : 1 } }) );
assert.commandWorked( admin.runCommand({ split : collSharded + "", middle : { _id : 0 } }) );
assert.commandWorked( admin.runCommand({ moveChunk : collSharded + "",
                                         find : { _id : 0 },
                                         to : shards[1]._id }) );

// Create the unsharded database
collUnsharded.insert({ some : "doc" });
assert.eq( null, collUnsharded.getDB().getLastError() );
collUnsharded.remove({});
assert.eq( null, collUnsharded.getDB().getLastError() );
printjson( admin.runCommand({ movePrimary : collUnsharded.getDB() + "", to : shards[0]._id }) );

// Make sure replica sets have a primary.
st.rs0.getPrimary();
st.rs1.getPrimary();
st.rs2.getPrimary();

st.printShardingStatus();

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

var mongosConnActive = new Mongo( mongos.host );
var mongosConnIdle = null;
var mongosConnNew = null;

mongosConnActive.getCollection( collSharded + "" ).insert({ _id : -1 });
mongosConnActive.getCollection( collSharded + "" ).insert({ _id : 1 });
assert.eq(null, mongosConnActive.getCollection( collSharded + "" ).getDB().getLastError());

mongosConnActive.getCollection( collUnsharded + "" ).insert({ _id : 1 });
assert.eq(null, mongosConnActive.getCollection( collUnsharded + "" ).getDB().getLastError());

jsTest.log("Stopping primary of third shard...");

mongosConnIdle = new Mongo( mongos.host );

st.rs2.stop(st.rs2.getPrimary(), true /*wait for stop*/ );

jsTest.log("Testing active connection with third primary down...");

assert.neq(null, mongosConnActive.getCollection( collSharded + "" ).findOne({ _id : -1 }));
assert.neq(null, mongosConnActive.getCollection( collSharded + "" ).findOne({ _id : 1 }));
assert.neq(null, mongosConnActive.getCollection( collUnsharded + "" ).findOne({ _id : 1 }));

mongosConnActive.getCollection( collSharded + "" ).insert({ _id : -2 });
assert.gleSuccess(mongosConnActive.getCollection( collSharded + "" ).getDB());
mongosConnActive.getCollection( collSharded + "" ).insert({ _id : 2 });
assert.gleSuccess(mongosConnActive.getCollection( collSharded + "" ).getDB());
mongosConnActive.getCollection( collUnsharded + "" ).insert({ _id : 2 });
assert.gleSuccess(mongosConnActive.getCollection( collUnsharded + "" ).getDB());

jsTest.log("Testing idle connection with third primary down...");

mongosConnIdle.getCollection( collSharded + "" ).insert({ _id : -3 });
assert.gleSuccess(mongosConnIdle.getCollection( collSharded + "" ).getDB());
mongosConnIdle.getCollection( collSharded + "" ).insert({ _id : 3 });
assert.gleSuccess(mongosConnIdle.getCollection( collSharded + "" ).getDB());
mongosConnIdle.getCollection( collUnsharded + "" ).insert({ _id : 3 });
assert.gleSuccess(mongosConnIdle.getCollection( collUnsharded + "" ).getDB());

assert.neq(null, mongosConnIdle.getCollection( collSharded + "" ).findOne({ _id : -1 }) );
assert.neq(null, mongosConnIdle.getCollection( collSharded + "" ).findOne({ _id : 1 }) );
assert.neq(null, mongosConnIdle.getCollection( collUnsharded + "" ).findOne({ _id : 1 }) );

jsTest.log("Testing new connections with third primary down...");

mongosConnNew = new Mongo( mongos.host );
assert.neq(null, mongosConnNew.getCollection( collSharded + "" ).findOne({ _id : -1 }) );
mongosConnNew = new Mongo( mongos.host );
assert.neq(null, mongosConnNew.getCollection( collSharded + "" ).findOne({ _id : 1 }) );
mongosConnNew = new Mongo( mongos.host );
assert.neq(null, mongosConnNew.getCollection( collUnsharded + "" ).findOne({ _id : 1 }) );

mongosConnNew = new Mongo( mongos.host );
mongosConnNew.getCollection( collSharded + "" ).insert({ _id : -4 });
assert.gleSuccess(mongosConnNew.getCollection( collSharded + "" ).getDB());
mongosConnNew = new Mongo( mongos.host );
mongosConnNew.getCollection( collSharded + "" ).insert({ _id : 4 });
assert.gleSuccess(mongosConnNew.getCollection( collSharded + "" ).getDB());
mongosConnNew = new Mongo( mongos.host );
mongosConnNew.getCollection( collUnsharded + "" ).insert({ _id : 4 });
assert.gleSuccess(mongosConnNew.getCollection( collUnsharded + "" ).getDB());

gc(); // Clean up new connections

mongosConnIdle = new Mongo( mongos.host );

jsTest.log("Stopping primary of second shard...");

mongosConnActive.setSlaveOk();
mongosConnIdle = new Mongo( mongos.host );
mongosConnIdle.setSlaveOk();

// Need to save this node for later
var rs1Secondary = st.rs1.getSecondary();

st.rs1.stop(st.rs1.getPrimary(), true /* wait for stop */);

jsTest.log("Testing active connection with second primary down...");

assert.neq(null, mongosConnActive.getCollection( collSharded + "" ).findOne({ _id : -1 }));
assert.neq(null, mongosConnActive.getCollection( collSharded + "" ).findOne({ _id : 1 }));
assert.neq(null, mongosConnActive.getCollection( collUnsharded + "" ).findOne({ _id : 1 }));

mongosConnActive.getCollection( collSharded + "" ).insert({ _id : -5 });
assert.gleSuccess(mongosConnActive.getCollection( collSharded + "" ).getDB());
mongosConnActive.getCollection( collSharded + "" ).insert({ _id : 5 });
gleErrorOrThrow(mongosConnActive.getCollection( collSharded + "" ).getDB());
mongosConnActive.getCollection( collUnsharded + "" ).insert({ _id : 5 });
assert.gleSuccess(mongosConnActive.getCollection( collUnsharded + "" ).getDB());

jsTest.log("Testing idle connection with second primary down...");

mongosConnIdle.getCollection( collSharded + "" ).insert({ _id : -6 });
assert.gleSuccess(mongosConnIdle.getCollection( collSharded + "" ).getDB());
assert.commandWorked(admin.runCommand({ setParameter : 1, logLevel : 5 }));
mongosConnIdle.getCollection( collSharded + "" ).insert({ _id : 6 });
gleErrorOrThrow(mongosConnIdle.getCollection( collSharded + "" ).getDB());
mongosConnIdle.getCollection( collUnsharded + "" ).insert({ _id : 6 });
assert.gleSuccess(mongosConnIdle.getCollection( collUnsharded + "" ).getDB());

assert.neq(null, mongosConnIdle.getCollection( collSharded + "" ).findOne({ _id : -1 }) );
assert.neq(null, mongosConnIdle.getCollection( collSharded + "" ).findOne({ _id : 1 }) );
assert.neq(null, mongosConnIdle.getCollection( collUnsharded + "" ).findOne({ _id : 1 }) );

jsTest.log("Testing new connections with second primary down...");

mongosConnNew = new Mongo( mongos.host );
mongosConnNew.setSlaveOk();
assert.neq(null, mongosConnNew.getCollection( collSharded + "" ).findOne({ _id : -1 }) );
mongosConnNew = new Mongo( mongos.host );
mongosConnNew.setSlaveOk();
assert.neq(null, mongosConnNew.getCollection( collSharded + "" ).findOne({ _id : 1 }) );
mongosConnNew = new Mongo( mongos.host );
mongosConnNew.setSlaveOk();
assert.neq(null, mongosConnNew.getCollection( collUnsharded + "" ).findOne({ _id : 1 }) );

mongosConnNew = new Mongo( mongos.host );
mongosConnNew.getCollection( collSharded + "" ).insert({ _id : -7 });
assert.gleSuccess(mongosConnNew.getCollection( collSharded + "" ).getDB());
mongosConnNew = new Mongo( mongos.host );
mongosConnNew.getCollection( collSharded + "" ).insert({ _id : 7 });
gleErrorOrThrow(mongosConnNew.getCollection( collSharded + "" ).getDB());
mongosConnNew = new Mongo( mongos.host );
mongosConnNew.getCollection( collUnsharded + "" ).insert({ _id : 7 });
assert.gleSuccess(mongosConnNew.getCollection( collUnsharded + "" ).getDB());

gc(); // Clean up new connections

jsTest.log("Stopping primary of first shard...");

mongosConnActive.setSlaveOk();
mongosConnIdle = new Mongo( mongos.host );
mongosConnIdle.setSlaveOk();

st.rs0.stop(st.rs0.getPrimary(), true /*wait for stop*/ );

jsTest.log("Testing active connection with first primary down...");

assert.neq(null, mongosConnActive.getCollection( collSharded + "" ).findOne({ _id : -1 }));
assert.neq(null, mongosConnActive.getCollection( collSharded + "" ).findOne({ _id : 1 }));
assert.neq(null, mongosConnActive.getCollection( collUnsharded + "" ).findOne({ _id : 1 }));

mongosConnActive.getCollection( collSharded + "" ).insert({ _id : -8 });
gleErrorOrThrow(mongosConnActive.getCollection( collSharded + "" ).getDB());
mongosConnActive.getCollection( collSharded + "" ).insert({ _id : 8 });
gleErrorOrThrow(mongosConnActive.getCollection( collSharded + "" ).getDB());
mongosConnActive.getCollection( collUnsharded + "" ).insert({ _id : 8 });
gleErrorOrThrow(mongosConnActive.getCollection( collUnsharded + "" ).getDB());

jsTest.log("Testing idle connection with first primary down...");

mongosConnIdle.getCollection( collSharded + "" ).insert({ _id : -9 });
gleErrorOrThrow(mongosConnIdle.getCollection( collSharded + "" ).getDB());
mongosConnIdle.getCollection( collSharded + "" ).insert({ _id : 9 });
gleErrorOrThrow(mongosConnIdle.getCollection( collSharded + "" ).getDB());
mongosConnIdle.getCollection( collUnsharded + "" ).insert({ _id : 9 });
gleErrorOrThrow(mongosConnIdle.getCollection( collUnsharded + "" ).getDB());

assert.neq(null, mongosConnIdle.getCollection( collSharded + "" ).findOne({ _id : -1 }) );
assert.neq(null, mongosConnIdle.getCollection( collSharded + "" ).findOne({ _id : 1 }) );
assert.neq(null, mongosConnIdle.getCollection( collUnsharded + "" ).findOne({ _id : 1 }) );

jsTest.log("Testing new connections with first primary down...");

mongosConnNew = new Mongo( mongos.host );
mongosConnNew.setSlaveOk();
assert.neq(null, mongosConnNew.getCollection( collSharded + "" ).findOne({ _id : -1 }) );
mongosConnNew = new Mongo( mongos.host );
mongosConnNew.setSlaveOk();
assert.neq(null, mongosConnNew.getCollection( collSharded + "" ).findOne({ _id : 1 }) );
mongosConnNew = new Mongo( mongos.host );
mongosConnNew.setSlaveOk();
assert.neq(null, mongosConnNew.getCollection( collUnsharded + "" ).findOne({ _id : 1 }) );

mongosConnNew = new Mongo( mongos.host );
mongosConnNew.getCollection( collSharded + "" ).insert({ _id : -10 });
gleErrorOrThrow(mongosConnNew.getCollection( collSharded + "" ).getDB());
mongosConnNew = new Mongo( mongos.host );
mongosConnNew.getCollection( collSharded + "" ).insert({ _id : 10 });
gleErrorOrThrow(mongosConnNew.getCollection( collSharded + "" ).getDB());
mongosConnNew = new Mongo( mongos.host );
mongosConnNew.getCollection( collUnsharded + "" ).insert({ _id : 10 });
gleErrorOrThrow(mongosConnNew.getCollection( collUnsharded + "" ).getDB());

gc(); // Clean up new connections

jsTest.log("Stopping second shard...");

mongosConnActive.setSlaveOk();
mongosConnIdle = new Mongo( mongos.host );
mongosConnIdle.setSlaveOk();

st.rs1.stop(rs1Secondary, true /* wait for stop */);

jsTest.log("Testing active connection with second shard down...");

assert.neq(null, mongosConnActive.getCollection( collSharded + "" ).findOne({ _id : -1 }));
assert.neq(null, mongosConnActive.getCollection( collUnsharded + "" ).findOne({ _id : 1 }));

mongosConnActive.getCollection( collSharded + "" ).insert({ _id : -8 });
gleErrorOrThrow(mongosConnActive.getCollection( collSharded + "" ).getDB());
mongosConnActive.getCollection( collSharded + "" ).insert({ _id : 8 });
gleErrorOrThrow(mongosConnActive.getCollection( collSharded + "" ).getDB());
mongosConnActive.getCollection( collUnsharded + "" ).insert({ _id : 8 });
gleErrorOrThrow(mongosConnActive.getCollection( collUnsharded + "" ).getDB());

jsTest.log("Testing idle connection with second shard down...");

mongosConnIdle.getCollection( collSharded + "" ).insert({ _id : -9 });
gleErrorOrThrow(mongosConnIdle.getCollection( collSharded + "" ).getDB());
mongosConnIdle.getCollection( collSharded + "" ).insert({ _id : 9 });
gleErrorOrThrow(mongosConnIdle.getCollection( collSharded + "" ).getDB());
mongosConnIdle.getCollection( collUnsharded + "" ).insert({ _id : 9 });
gleErrorOrThrow(mongosConnIdle.getCollection( collUnsharded + "" ).getDB());

assert.neq(null, mongosConnIdle.getCollection( collSharded + "" ).findOne({ _id : -1 }) );
assert.neq(null, mongosConnIdle.getCollection( collUnsharded + "" ).findOne({ _id : 1 }) );

jsTest.log("Testing new connections with second shard down...");

mongosConnNew = new Mongo( mongos.host );
mongosConnNew.setSlaveOk();
assert.neq(null, mongosConnNew.getCollection( collSharded + "" ).findOne({ _id : -1 }) );
mongosConnNew = new Mongo( mongos.host );
mongosConnNew.setSlaveOk();
assert.neq(null, mongosConnNew.getCollection( collUnsharded + "" ).findOne({ _id : 1 }) );

mongosConnNew = new Mongo( mongos.host );
mongosConnNew.getCollection( collSharded + "" ).insert({ _id : -10 });
gleErrorOrThrow(mongosConnNew.getCollection( collSharded + "" ).getDB());
mongosConnNew = new Mongo( mongos.host );
mongosConnNew.getCollection( collSharded + "" ).insert({ _id : 10 });
gleErrorOrThrow(mongosConnNew.getCollection( collSharded + "" ).getDB());
mongosConnNew = new Mongo( mongos.host );
mongosConnNew.getCollection( collUnsharded + "" ).insert({ _id : 10 });
gleErrorOrThrow(mongosConnNew.getCollection( collUnsharded + "" ).getDB());

gc(); // Clean up new connections

jsTest.log("DONE!");
st.stop();





