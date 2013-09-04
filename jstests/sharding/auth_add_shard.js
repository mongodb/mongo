
/* SERVER-5124
 * The puporse of this test is to test authentication when adding/removing a shard 
 * The test sets up a sharded system, then adds/remove a shard.
 */ 

// login method to login into the database
function login(userObj) {
    var authResult = mongos.getDB(userObj.db).auth(userObj.username, userObj.password); 
    printjson(authResult);
}

// admin user object
adminUser = { db : "admin", username : "foo", password : "bar" };

//set up a 2 shard cluster with keyfile
var st = new ShardingTest( { name : "auth_add_shard1", shards : 1,
                            mongos : 1, verbose : 1, keyFile : "jstests/libs/key1" } )
st.stopBalancer();

var mongos = st.s0
var admin = mongos.getDB("admin")

assert.eq( 1, st.config.shards.count() , "initial server count wrong" );

print("1 shard system setup");

//add the admin user
var user = admin.system.users.findOne();
if (user) {
    print("user already exists");
    printjson(user);
}
else {
    print("adding user");
    mongos.getDB(adminUser.db).addUser(adminUser.username, adminUser.password, jsTest.adminUserRoles);
}

//login as admin user
login(adminUser);

//start a mongod with NO keyfile
var conn = MongoRunner.runMongod({});
print (conn);

// --------------- Test 1 --------------------
// Add shard to the existing cluster
var result = admin.runCommand( {addShard : conn.host} );
printjson(result);
// make sure the shard wasn't added
assert.eq(result.ok, 0, "added shard without keyfile");
// stop mongod
MongoRunner.stopMongod( conn );

//--------------- Test 2 --------------------
//start mongod again, this time with keyfile 
var conn = MongoRunner.runMongod( {keyFile : "jstests/libs/key1"} );
//try adding the new shard
var result = admin.runCommand( {addShard : conn.host} );
printjson(result);
//make sure the shard was added successfully
assert.eq(result.ok, 1, "failed to add shard with keyfile");

//Add some data
var db = mongos.getDB("foo");
var collA = mongos.getCollection("foo.bar")

// enable sharding on a collection
printjson( admin.runCommand( { enableSharding : "" + collA.getDB() } ) )
printjson( admin.runCommand( { movePrimary : "foo", to : "shard0000" } ) );

admin.runCommand( { shardCollection : "" + collA, key : { _id : 1 } } )

// add data to the sharded collection
for (i=0; i<4; i++) {
  db.bar.save({_id:i});
  printjson(admin.runCommand( { split : "" + collA, middle : { _id : i } }) )
}
// move a chunk 
printjson( admin.runCommand( { moveChunk : "foo.bar", find : { _id : 1 }, to : "shard0001" }) )

//verify the chunk was moved
admin.runCommand( { flushRouterConfig : 1 } )
var config = mongos.getDB("config")
config.printShardingStatus(true)

// start balancer before removing the shard
st.startBalancer();

//--------------- Test 3 --------------------
// now drain the shard
var result = admin.runCommand( {removeShard : conn.host} );
printjson(result);
assert.eq(result.ok, 1, "failed to start draining shard");

// give it some time to drain
assert.soon(function() {
    var result = admin.runCommand( {removeShard : conn.host} );
    printjson(result);
    return result.ok && result.state == "completed"
}, "failed to drain shard completely", 5 * 60 * 1000)

assert.eq( 1, st.config.shards.count() , "removed server still appears in count" );

MongoRunner.stopMongod( conn );
st.stop();
