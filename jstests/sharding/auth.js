
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

var s = new ShardingTest( "auth1", 0 , 0 , 1 , {rs: true, chunksize : 2, extraOptions : {"keyFile" : "jstests/libs/key1"}});

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

d1 = new ReplSetTest({name : "d1", nodes : 3, startPort : 34000});
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

print("insert try 1");
s.getDB("test").foo.insert({x:1});
result = s.getDB("test").runCommand({getLastError : 1});
printjson(result);

logout(adminUser);

login(testUser);

print("insert try 2");
s.getDB("test").foo.insert({x:1});
result = s.getDB("test").runCommand({getLastError : 1});
printjson(result);

login(adminUser);

d2 = new ReplSetTest({name : "d2", nodes : 3, startPort : 36000});
d2.startSet({keyFile : "jstests/libs/key1"});
d2.initiate();

shardName = getShardName(d2);

result = s.adminCommand({addShard : shardName})

for (i=0; i<100000; i++) {
    s.getDB("test").foo.insert({x:i, abc : "defg", date : new Date(), str : "all the talk on the market"});
}

var d1Chunks = s.getDB("config").chunks.count({shard : "d1"});
var d2Chunks = s.getDB("config").chunks.count({shard : "d2"});
var totalChunks = s.getDB("config").chunks.count({ns : "test.foo"});

assert(d1Chunks > 0 && d2Chunks > 0 && d1Chunks+d2Chunks == totalChunks);

assert.eq(s.getDB("test").foo.count(), 100001);

