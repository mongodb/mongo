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

function login(userObj) {
    var n = s.getDB(userObj.db).runCommand({getnonce: 1});
    var a = s.getDB(userObj.db).runCommand({authenticate: 1, user: userObj.username, nonce: n.nonce, key: s.getDB("admin").__pwHash(n.nonce, userObj.username, userObj.password)});
    printjson(a);
}

function logout(userObj) {
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
    s.getDB(adminUser.db).addUser(adminUser.username, adminUser.password);
}

login(adminUser);
s.getDB( "config" ).settings.update( { _id : "chunksize" }, {$set : {value : 1 }}, true );
printjson(s.getDB("config").runCommand({getlasterror:1}));
printjson(s.getDB("config").settings.find().toArray());

print("restart mongos");
stopMongoProgram(31000);
var opts = { port : 31000, v : 0, configdb : s._configDB, keyFile : "jstests/libs/key1", chunkSize : 1 };
var conn = startMongos( opts );
s.s = s._mongos[0] = s["s0"] = conn;

login(adminUser);

d1 = new ReplSetTest({name : "d1", nodes : 3, startPort : 31100});
d1.startSet({keyFile : "jstests/libs/key2"});
d1.initiate();

print("initiated");
var shardName = getShardName(d1);

print("adding shard w/out auth "+shardName);
logout(adminUser);

var result = s.getDB("admin").runCommand({addShard : shardName});
printjson(result);
assert.eq(result.errmsg, "unauthorized");

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
d1.startSet({keyFile : "jstests/libs/key1"});
d1.initiate();
var master = d1.getMaster();

print("adding shard w/auth "+shardName);

result = s.getDB("admin").runCommand({addShard : shardName});
assert.eq(result.ok, 1, tojson(result));

s.getDB("admin").runCommand({enableSharding : "test"});
s.getDB("admin").runCommand({shardCollection : "test.foo", key : {x : 1}});

s.getDB(testUser.db).addUser(testUser.username, testUser.password);

logout(adminUser);

print("query try");
var e = assert.throws(function() {
    conn.getDB("foo").bar.findOne();
});
printjson(e);

print("cmd try");
e = assert.throws(function() {
    conn.getDB("foo").runCommand({listdbs:1});
});
printjson(e);

print("insert try 1");
s.getDB("test").foo.insert({x:1});

login(testUser);
assert.eq(s.getDB("test").foo.findOne(), null);

print("insert try 2");
s.getDB("test").foo.insert({x:1});
result = s.getDB("test").runCommand({getLastError : 1});
assert.eq(result.err, null);

logout(testUser);

d2 = new ReplSetTest({name : "d2", nodes : 3, startPort : 31200});
d2.startSet({keyFile : "jstests/libs/key1"});
d2.initiate();

shardName = getShardName(d2);

print("adding shard "+shardName);
login(adminUser);
print("logged in");
result = s.getDB("admin").runCommand({addShard : shardName})

var num = 100000;
for (i=0; i<num; i++) {
    s.getDB("test").foo.insert({x:i, abc : "defg", date : new Date(), str : "all the talk on the market"});
}

var d1Chunks = s.getDB("config").chunks.count({shard : "d1"});
var d2Chunks = s.getDB("config").chunks.count({shard : "d2"});
var totalChunks = s.getDB("config").chunks.count({ns : "test.foo"});

print("chunks: " + d1Chunks+" "+d2Chunks+" "+totalChunks);

assert(d1Chunks > 0 && d2Chunks > 0 && d1Chunks+d2Chunks == totalChunks);

//SERVER-3645
//assert.eq(s.getDB("test").foo.count(), num+1);
assert.eq(s.getDB("test").foo.find().itcount(), num+1);

s.s.setSlaveOk();

// We're only sure we aren't duplicating documents iff there's no balancing going on here
// This call also waits for any ongoing balancing to stop
sh.stopBalancer()

var cursor = s.getDB("test").foo.find({x:{$lt : 500}});

var count = 0;
while (cursor.hasNext()) {
    cursor.next();
    count++;
}

assert.eq(count, 501);

// check that dump doesn't get stuck with auth
var x = runMongoProgram( "mongodump", "--host", "127.0.0.1:31000", "-d", testUser.db, "-u", testUser.username, "-p", testUser.password);

print("result: "+x);


s.stop();
