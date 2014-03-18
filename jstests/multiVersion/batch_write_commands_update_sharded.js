/**
 * This test checks update write command corner cases:
 *   -- nModified behavior
 *   -- other?
 */

// Options for a cluster with two one-node replica set shards, shard0 is v2.4, shard1 is v2.6
var options = { separateConfig : true,
                rs : true,
                configOptions : { binVersion : "2.6" },
                rsOptions : { nodes : 1,
                              nojournal : "" },
                // Options for each replica set shard
                rs0 : { binVersion : "2.4" },
                rs1 : { binVersion : "2.6" },
                mongosOptions : { binVersion : "2.6" } };

var st = new ShardingTest({ shards : 2, mongos : 1, other : options });
st.stopBalancer();

var mongos = st.s0;
var admin = mongos.getDB("admin");
var shards = mongos.getCollection("config.shards").find().toArray();
var coll24 = mongos.getCollection("test24.coll24");
var collMixed = mongos.getCollection("testMixed.collMixed");
var coll26 = mongos.getCollection("test26.coll26");

// Move the primary of the collections to the appropriate shards
coll24.drop();
collMixed.drop();
coll26.drop();
printjson(admin.runCommand({ movePrimary : coll24.getDB().toString(),
                             to : shards[0]._id }));
printjson(admin.runCommand({ movePrimary : collMixed.getDB().toString(),
                             to : shards[0]._id }));
printjson(admin.runCommand({ movePrimary : coll26.getDB().toString(),
                             to : shards[1]._id }));

// The mixed collection spans shards
assert.commandWorked(admin.runCommand({ enableSharding : collMixed.getDB().toString() }));
assert.commandWorked(admin.runCommand({ shardCollection : collMixed.toString(),
                                        key : { _id : 1 } }));
assert.commandWorked(admin.runCommand({ split : collMixed.toString(),
                                        middle : { _id : 0 } }));
assert.commandWorked(admin.runCommand({ moveChunk : collMixed.toString(),
                                        find : { _id : 0 },
                                        to : shards[1]._id }));

st.printShardingStatus();


// Add test data, 20 docs per collection with _id:[-5, 5]
var colls = [coll24, collMixed, coll26]
colls.forEach(function(coll) {
    for(x = -5; x <= 5; x++) {
        coll.insert({_id:x}, {writeConcern:{w:1}});
    }
    assert.eq(11, coll.count());
})

// Test nModified behavior

// 2.6 mongod shard
var req = {update:coll26.getName(),
           updates:[
                {q:{}, u:{$inc:{a:1}}, multi:true},
                {q:{}, u:{$set:{a:1}}, multi:true}
           ]
       };
var res = assert.commandWorked(coll26.getDB().runCommand(req));
assert.eq(11, res.nModified, "coll26: " + tojson(res))
assert.eq(22, res.n,  "coll26: " + tojson(res))

// 2.4 mongod shard
var req = {update:coll24.getName(),
           updates:[
                {q:{}, u:{$set:{a:1}}, multi:true},
                {q:{}, u:{$set:{a:1}}, multi:true}
           ]
       };
var res = assert.commandWorked(coll24.getDB().runCommand(req));
assert.eq(undefined, res.nModified, tojson(res))
assert.eq(22, res.n, tojson(res))

// mixed version mongod shards
var req = {update:collMixed.getName(),
           updates:[
                {q:{}, u:{$set:{a:1}}, multi:true},
                {q:{}, u:{$set:{a:1}}, multi:true}
           ]
       };
var res = assert.commandWorked(collMixed.getDB().runCommand(req));
assert.eq(undefined, res.nModified, tojson(res))
assert.eq(22, res.n, tojson(res))

// mixed version mongod shards, only hitting 2.6
var req = {update:collMixed.getName(),
           updates:[
                {q:{_id:1}, u:{$set:{a:1}}},
           ]
       };
var res = assert.commandWorked(collMixed.getDB().runCommand(req));
assert.eq(0, res.nModified, tojson(res))
assert.eq(1, res.n, tojson(res))


// mixed version mongod shards, only hitting 2.4
var req = {update:collMixed.getName(),
           updates:[
                {q:{_id:-1}, u:{$set:{a:1}}},
           ]
       };
var res = assert.commandWorked(collMixed.getDB().runCommand(req));
assert.eq(undefined, res.nModified, tojson(res))
assert.eq(1, res.n, tojson(res))

// mixed version mongod shards, only hitting both
var req = {update:collMixed.getName(),
           updates:[
                {q:{_id:-1}, u:{$set:{a:1}}},
                {q:{_id:1}, u:{$set:{a:1}}},
           ]
       };
var res = assert.commandWorked(collMixed.getDB().runCommand(req));
assert.eq(undefined, res.nModified, tojson(res))
assert.eq(2, res.n, tojson(res))

st.stop();

jsTest.log("DONE!");

