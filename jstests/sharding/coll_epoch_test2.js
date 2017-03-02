// Tests that resharding a collection is detected correctly by all operation types
//
// The idea here is that a collection may be resharded / unsharded at any point, and any type of
// operation on a bongos may be active when it happens.  All operations should handle gracefully.
//

var st = new ShardingTest({shards: 2, bongos: 5, verbose: 1});
// Balancer is by default stopped, thus it will not interfere

// Use separate bongos for reading, updating, inserting, removing data
var readBongos = st.s1;
var updateBongos = st.s2;
var insertBongos = st.s3;
var removeBongos = st.s4;

var config = st.s.getDB("config");
var admin = st.s.getDB("admin");
var coll = st.s.getCollection("foo.bar");

insertBongos.getDB("admin").runCommand({setParameter: 1, traceExceptions: true});

var shards = {};
config.shards.find().forEach(function(doc) {
    shards[doc._id] = new Bongo(doc.host);
});

//
// Set up a sharded collection
//

jsTest.log("Enabling sharding for the first time...");

admin.runCommand({enableSharding: coll.getDB() + ""});
st.ensurePrimaryShard(coll.getDB().getName(), 'shard0001');
admin.runCommand({shardCollection: coll + "", key: {_id: 1}});

assert.writeOK(coll.insert({hello: "world"}));

jsTest.log("Sharding collection across multiple shards...");

var getOtherShard = function(shard) {
    for (id in shards) {
        if (id != shard)
            return id;
    }
};

printjson(admin.runCommand({split: coll + "", middle: {_id: 0}}));
printjson(admin.runCommand({
    moveChunk: coll + "",
    find: {_id: 0},
    to: getOtherShard(config.databases.findOne({_id: coll.getDB() + ""}).primary)
}));

st.printShardingStatus();

//
// Force all bongoses to load the current status of the cluster
//

jsTest.log("Loading this status in all bongoses...");

for (var i = 0; i < st._bongos.length; i++) {
    printjson(st._bongos[i].getDB("admin").runCommand({flushRouterConfig: 1}));
    assert.neq(null, st._bongos[i].getCollection(coll + "").findOne());
}

//
// Drop and recreate a new sharded collection in the same namespace, where the shard and collection
// versions are the same, but the split is at a different point.
//

jsTest.log("Rebuilding sharded collection with different split...");

coll.drop();

var droppedCollDoc = config.collections.findOne({_id: coll.getFullName()});
assert(droppedCollDoc != null);
assert.eq(true, droppedCollDoc.dropped);
assert(droppedCollDoc.lastmodEpoch != null);
assert(droppedCollDoc.lastmodEpoch.equals(new ObjectId("000000000000000000000000")),
       "epoch not zero: " + droppedCollDoc.lastmodEpoch);

admin.runCommand({enableSharding: coll.getDB() + ""});
st.ensurePrimaryShard(coll.getDB().getName(), 'shard0001');
admin.runCommand({shardCollection: coll + "", key: {_id: 1}});

var bulk = coll.initializeUnorderedBulkOp();
for (var i = 0; i < 100; i++)
    bulk.insert({_id: i});
assert.writeOK(bulk.execute());

printjson(admin.runCommand({split: coll + "", middle: {_id: 200}}));
printjson(admin.runCommand({
    moveChunk: coll + "",
    find: {_id: 200},
    to: getOtherShard(config.databases.findOne({_id: coll.getDB() + ""}).primary)
}));

//
// Make sure all operations on bongoses aren't tricked by the change
//

jsTest.log("Checking other bongoses for detection of change...");

jsTest.log("Checking find...");
// Ensure that finding an element works when resharding
assert.neq(null, readBongos.getCollection(coll + "").findOne({_id: 1}));

jsTest.log("Checking update...");
// Ensure that updating an element finds the right location
assert.writeOK(updateBongos.getCollection(coll + "").update({_id: 1}, {$set: {updated: true}}));
assert.neq(null, coll.findOne({updated: true}));

jsTest.log("Checking insert...");
// Ensure that inserting an element finds the right shard
assert.writeOK(insertBongos.getCollection(coll + "").insert({_id: 101}));
assert.neq(null, coll.findOne({_id: 101}));

jsTest.log("Checking remove...");
// Ensure that removing an element finds the right shard, verified by the bongos doing the sharding
assert.writeOK(removeBongos.getCollection(coll + "").remove({_id: 2}));
assert.eq(null, coll.findOne({_id: 2}));

coll.drop();

jsTest.log("Done!");

st.stop();
