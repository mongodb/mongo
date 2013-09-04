adminUser = {
    db : "admin",
    username : "foo",
    password : "bar"
};

testUser = {
    db : "test",
    username : "bar",
    password : "baz"
};

testUserReadOnly = {
    db : "test",
    username : "sad",
    password : "bat"
};


function login(userObj , thingToUse ) {
    if ( ! thingToUse )
        thingToUse = s;
    
    var n = thingToUse.getDB(userObj.db).runCommand({getnonce: 1});
    var a = thingToUse.getDB(userObj.db).runCommand({authenticate: 1, user: userObj.username, nonce: n.nonce, key: thingToUse.getDB("admin").__pwHash(n.nonce, userObj.username, userObj.password)});
    printjson(a);
}

function logout(userObj, thingToUse ) {
    if ( ! thingToUse )
        thingToUse = s;

    s.getDB(userObj.db).runCommand({logout:1});
}

function getShardName(rsTest) {
    var master = rsTest.getMaster();
    var config = master.getDB("local").system.replset.findOne();
    var members = config.members.map(function(elem) { return elem.host; });
    return config._id+"/"+members.join(",");
}

var s = new ShardingTest( "auth1", 0 , 0 , 1 , {rs: true, extraOptions : {"keyFile" : "jstests/libs/key1"}, noChunkSize : true});

print("logging in first, if there was an unclean shutdown the user might already exist");
login(adminUser);

var user = s.getDB("admin").system.users.findOne();
if (user) {
    print("user already exists");
    printjson(user);
}
else {
    print("adding user");
    s.getDB(adminUser.db).addUser(adminUser.username, adminUser.password, jsTest.adminUserRoles);
}

login(adminUser);
s.getDB( "config" ).settings.update( { _id : "chunksize" }, {$set : {value : 1 }}, true );
printjson(s.getDB("config").runCommand({getlasterror:1}));
printjson(s.getDB("config").settings.find().toArray());

print("restart mongos");
stopMongoProgram(31000);
var opts = { port : 31000, v : 2, configdb : s._configDB, keyFile : "jstests/libs/key1", chunkSize : 1 };
var conn = startMongos( opts );
s.s = s._mongos[0] = s["s0"] = conn;

login(adminUser);

d1 = new ReplSetTest({name : "d1", nodes : 3, startPort : 31100, useHostName : true });
d1.startSet({keyFile : "jstests/libs/key2", verbose : 0});
d1.initiate();

print("initiated");
var shardName = getShardName(d1);

print("adding shard w/out auth "+shardName);
logout(adminUser);

var result = s.getDB("admin").runCommand({addShard : shardName});
printjson(result);
assert.eq(result.code, 13);

login(adminUser);

print("adding shard w/wrong key "+shardName);

var thrown = false;
try {
    result = s.adminCommand({addShard : shardName});
}
catch(e) {
    thrown = true;
    printjson(e);
}
assert(thrown);

print("start rs w/correct key");
d1.stopSet();
d1.startSet({keyFile : "jstests/libs/key1", verbose : 0});
d1.initiate();
var master = d1.getMaster();

print("adding shard w/auth "+shardName);

result = s.getDB("admin").runCommand({addShard : shardName});
assert.eq(result.ok, 1, tojson(result));

s.getDB("admin").runCommand({enableSharding : "test"});
s.getDB("admin").runCommand({shardCollection : "test.foo", key : {x : 1}});

d1.waitForState( d1.getSecondaries(), d1.SECONDARY, 5 * 60 * 1000 )

s.getDB(testUser.db).addUser(testUser.username, testUser.password , jsTest.basicUserRoles, 3 )
s.getDB(testUserReadOnly.db).addUser(testUserReadOnly.username,
                                     testUserReadOnly.password,
                                     jsTest.readOnlyUserRoles,
                                     3);

logout(adminUser);

print("query try");
var e = assert.throws(function() {
    conn.getDB("foo").bar.findOne();
});
printjson(e);

print("cmd try");
assert.eq( 0, conn.getDB("foo").runCommand({listDatabases:1}).ok );

print("insert try 1");
s.getDB("test").foo.insert({x:1});

login(testUser);
assert.eq(s.getDB("test").foo.findOne(), null);

print("insert try 2");
s.getDB("test").foo.insert({x:1});
result = s.getDB("test").getLastErrorObj();
assert.eq(result.err, null);
assert.eq( 1 , s.getDB( "test" ).foo.find().itcount() , tojson(result) );

logout(testUser);

d2 = new ReplSetTest({name : "d2", nodes : 3, startPort : 31200, useHostName : true });
d2.startSet({keyFile : "jstests/libs/key1", verbose : 0});
d2.initiate();
d2.awaitSecondaryNodes();

shardName = getShardName(d2);

print("adding shard "+shardName);
login(adminUser);
print("logged in");
result = s.getDB("admin").runCommand({addShard : shardName})

ReplSetTest.awaitRSClientHosts(s.s, d1.nodes, {ok: true });
ReplSetTest.awaitRSClientHosts(s.s, d2.nodes, {ok: true });

s.getDB("test").foo.remove({})

var num = 100000;
for (i=0; i<num; i++) {
    s.getDB("test").foo.insert({_id:i, x:i, abc : "defg", date : new Date(), str : "all the talk on the market"});
}

// Make sure all data gets sent through
printjson( s.getDB("test").getLastError() )
for (var i = 0; i < s._connections.length; i++) { // SERVER-4356
    s._connections[i].getDB("test").getLastError();
}

var d1Chunks = s.getDB("config").chunks.count({shard : "d1"});
var d2Chunks = s.getDB("config").chunks.count({shard : "d2"});
var totalChunks = s.getDB("config").chunks.count({ns : "test.foo"});

print("chunks: " + d1Chunks+" "+d2Chunks+" "+totalChunks);

assert(d1Chunks > 0 && d2Chunks > 0 && d1Chunks+d2Chunks == totalChunks);

//SERVER-3645
//assert.eq(s.getDB("test").foo.count(), num+1);
var numDocs = s.getDB("test").foo.find().itcount()
if (numDocs != num) {
    // Missing documents. At this point we're already in a failure mode, the code in this statement
    // is to get a better idea how/why it's failing.

    var numDocsSeen = 0;
    var lastDocNumber = -1;
    var missingDocNumbers = [];
    var docs = s.getDB("test").foo.find().sort({x:1}).toArray();
    for (var i = 0; i < docs.length; i++) {
        if (docs[i].x != lastDocNumber + 1) {
            for (var missing = lastDocNumber + 1; missing < docs[i].x; missing++) {
                missingDocNumbers.push(missing);
            }
        }
        lastDocNumber = docs[i].x;
        numDocsSeen++;
    }
    assert.eq(numDocs, numDocsSeen, "More docs discovered on second find() even though getLastError was already called")
    assert.eq(num - numDocs, missingDocNumbers.length);

    load('jstests/libs/trace_missing_docs.js');
    
    for ( var i = 0; i < missingDocNumbers.length; i++ ) {
        jsTest.log( "Tracing doc: " + missingDocNumbers[i] );
        traceMissingDoc( s.getDB( "test" ).foo, { _id : missingDocNumbers[i], 
                                                    x : missingDocNumbers[i] } );
    }
    
    assert(false, "Number of docs found does not equal the number inserted. Missing docs: " + missingDocNumbers);
}


//SERVER-4031
/*
s.s.setSlaveOk();

// We're only sure we aren't duplicating documents iff there's no balancing going on here
// This call also waits for any ongoing balancing to stop
s.stopBalancer()
*/

var cursor = s.getDB("test").foo.find({x:{$lt : 500}});

var count = 0;
while (cursor.hasNext()) {
    cursor.next();
    count++;
}

assert.eq(count, 500);

logout(adminUser);

d2.waitForState( d2.getSecondaries(), d2.SECONDARY, 5 * 60 * 1000 )

// add admin on shard itself, hack to prevent localhost auth bypass
d1.getMaster().getDB(adminUser.db).addUser(adminUser.username,
                                           adminUser.password,
                                           jsTest.adminUserRoles, 3);
d2.getMaster().getDB(adminUser.db).addUser(adminUser.username,
                                           adminUser.password,
                                           jsTest.adminUserRoles, 3);

login(testUser);
print( "testing map reduce" );
/* sharded map reduce can be tricky since all components talk to each other.
   for example SERVER-4114 is triggered when 1 mongod connects to another for final reduce
   it's not properly tested here since addresses are localhost, which is more permissive */
var res = s.getDB("test").runCommand(
    {mapreduce : "foo",
     map : function() { emit(this.x, 1); },
     reduce : function(key, values) { return values.length; },
     out:"mrout"
    });
printjson(res);
assert.commandWorked(res);

// check that dump doesn't get stuck with auth
var x = runMongoProgram( "mongodump", "--host", "127.0.0.1:31000", "-d", testUser.db, "-u", testUser.username, "-p", testUser.password);

print("result: "+x);

// test read only users

print( "starting read only tests" );

readOnlyS = new Mongo( s.getDB( "test" ).getMongo().host )
readOnlyDB = readOnlyS.getDB( "test" );

print( "   testing find that should fail" );
assert.throws( function(){ readOnlyDB.foo.findOne(); } )

print( "   logging in" );
login( testUserReadOnly , readOnlyS );

print( "   testing find that should work" );
readOnlyDB.foo.findOne();

print( "   testing write that should fail" );
readOnlyDB.foo.insert( { eliot : 1 } );
result = readOnlyDB.getLastError();
assert( ! result.ok , tojson( result ) )

print( "   testing read command (should succeed)" );
assert.commandWorked(readOnlyDB.runCommand({count : "foo"}));

print("make sure currentOp/killOp fail");
assert.throws(function() {
    printjson(readOnlyDB.currentOp());
});
assert.throws(function() {
    printjson(readOnlyDB.killOp(123));
});
// fsyncUnlock doesn't work in mongos anyway, so no need check authorization for it

/*
broken because of SERVER-4156
print( "   testing write command (should fail)" );
assert.commandFailed(readOnlyDB.runCommand(
    {mapreduce : "foo",
     map : function() { emit(this.y, 1); },
     reduce : function(key, values) { return values.length; },
     out:"blarg"
    }));
*/
print( "   testing logout (should succeed)" );
assert.commandWorked(readOnlyDB.runCommand({logout : 1}));

print("make sure currentOp/killOp fail again");
assert.throws(function() {
    printjson(readOnlyDB.currentOp());
});
assert.throws(function() {
    printjson(readOnlyDB.killOp(123));
});
// fsyncUnlock doesn't work in mongos anyway, so no need check authorization for it

s.stop();
