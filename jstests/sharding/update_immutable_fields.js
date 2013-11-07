// Tests that updates can't change immutable fields (used in sharded system)
var st = new ShardingTest({shards : 2,
                           mongos : 1,
                           verbose : 0,
                           other : {separateConfig : 1}})
st.stopBalancer();

var mongos = st.s;
var config = mongos.getDB("config");
var coll = mongos.getCollection(jsTestName() + ".coll1");
var shard0 = st.shard0;

printjson(config.adminCommand({enableSharding : coll.getDB() + ""}))
printjson(config.adminCommand({shardCollection : "" + coll, key : {a : 1}}))

var getDirectShardedConn = function( st, collName ) {

    var shardConnWithVersion = new Mongo( st.shard0.host );

    var mockServerId = new ObjectId();
    var configConnStr = st._configDB;

    var maxChunk = st.s0.getCollection( "config.chunks" )
                   .find({ ns : collName }).sort({ lastmod : -1 }).next();
    
    var ssvInitCmd = { setShardVersion : collName, 
                       configdb : configConnStr, 
                       serverID : mockServerId,
                       version : maxChunk.lastmod,
                       versionEpoch : maxChunk.lastmodEpoch };
    
    printjson( ssvInitCmd );

    assert.commandWorked( shardConnWithVersion.getDB( "admin" ).runCommand( ssvInitCmd ) );
    
    return shardConnWithVersion;
}

var shard0Coll = getDirectShardedConn(st, coll.getFullName()).getCollection(coll.getFullName());

// No shard key
shard0Coll.remove()
shard0Coll.save({_id:3})
assert.gleError(shard0Coll.getDB(), function(gle) {
    return "save without shard key passed - " + tojson(gle) + " doc: " + tojson(shard0Coll.findOne()) 
});

// Full shard key in save
shard0Coll.save({_id: 1, a: 1})
assert.gleSuccess(shard0Coll.getDB(), "save with shard key failed");

// Full shard key on replacement (basically the same as above)
shard0Coll.remove()
shard0Coll.update({_id: 1}, {a:1}, true)
assert.gleSuccess(shard0Coll.getDB(), "update + upsert (replacement) with shard key failed"); 

// Full shard key after $set
shard0Coll.remove()
shard0Coll.update({_id: 1}, {$set: {a: 1}}, true)
assert.gleSuccess(shard0Coll.getDB(), "update + upsert ($set) with shard key failed"); 

// Update existing doc (replacement), same shard key value
shard0Coll.update({_id: 1}, {a:1})
assert.gleSuccess(shard0Coll.getDB(), "update (replacement) with shard key failed"); 

//Update existing doc ($set), same shard key value
shard0Coll.update({_id: 1}, {$set: {a: 1}})
assert.gleSuccess(shard0Coll.getDB(), "update ($set) with shard key failed"); 

// Error due to mutating the shard key (replacement)
shard0Coll.update({_id: 1}, {b:1})
assert.gleError(shard0Coll.getDB(), "update (replacement) removes shard key"); 

// Error due to mutating the shard key ($set)
shard0Coll.update({_id: 1}, {$unset: {a: 1}})
assert.gleError(shard0Coll.getDB(), "update ($unset) removes shard key"); 

// Error due to removing all the embedded fields.
shard0Coll.remove()

shard0Coll.save({_id: 2, a:{c:1, b:1}})
assert.gleSuccess(shard0Coll.getDB(), "save with shard key failed -- 1"); 

shard0Coll.update({}, {$unset: {"a.c": 1}})
assert.gleError(shard0Coll.getDB(), function(gle) {
    return "unsetting part of shard key passed - " + tojson(gle) + 
            " doc: " + tojson(shard0Coll.findOne()) 
});

shard0Coll.update({}, {$unset: {"a.b": 1, "a.c": 1}})
assert.gleError(shard0Coll.getDB(), function(gle) {
    return "unsetting nested fields of shard key passed - " + tojson(gle) + 
            " doc: " + tojson(shard0Coll.findOne()) 
});

db.adminCommand("unsetSharding");
jsTest.log("DONE!"); // distinguishes shutdown failures
st.stop();
