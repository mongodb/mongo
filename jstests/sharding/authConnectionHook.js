// Test for SERVER-8786 - if the first operation on an authenticated shard is moveChunk, it breaks the cluster.
var st = new ShardingTest({ keyFile : 'jstests/libs/key1', shards : 2, chunksize : 1, config : 3, verbose : 2,
                            other : { nopreallocj : 1, verbose : 2, useHostname : true,
                                      configOptions : { verbose : 2 }}});

var mongos = st.s;
var adminDB = mongos.getDB('admin');
var db = mongos.getDB('test')

adminDB.addUser('admin', 'password', jsTest.adminUserRoles);

adminDB.auth('admin', 'password');

adminDB.runCommand({enableSharding : "test"});
adminDB.runCommand({shardCollection : "test.foo", key : {x : 1}});
st.stopBalancer();

for (var i = 0; i < 100; i++) {
    db.foo.insert({x:i});
}

adminDB.runCommand({split: "test.foo", middle: {x:50}});
var curShard = st.getShard("test.foo", {x:75});
var otherShard = st.getOther(curShard).name;
adminDB.runCommand({moveChunk: "test.foo", find: {x:25}, to: otherShard, _waitForDelete:true});

st.printShardingStatus();

MongoRunner.stopMongod(st.shard0);
st.shard0 = MongoRunner.runMongod({restart: st.shard0});

// May fail the first couple times due to socket exceptions
assert.soon( function() {
                 var res = adminDB.runCommand({moveChunk: "test.foo",
                                               find: {x:75},
                                               to: otherShard});
                 printjson(res);
                 return res.ok;
             });


printjson(db.foo.findOne({x:25}));
printjson(db.foo.findOne({x:75}));

st.stop();