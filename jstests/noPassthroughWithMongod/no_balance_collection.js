// Tests whether the noBalance flag disables balancing for collections

var st = new ShardingTest({shards: 2, mongos: 1});

// First, test that shell helpers require an argument
assert.throws(sh.disableBalancing, [], "sh.disableBalancing requires a collection");
assert.throws(sh.enableBalancing, [], "sh.enableBalancing requires a collection");

var shardAName = st.shard0.shardName;
var shardBName = st.shard1.shardName;

var collA = st.s.getCollection(jsTest.name() + ".collA");
var collB = st.s.getCollection(jsTest.name() + ".collB");

// Shard two collections
st.shardColl(collA, {_id: 1}, false);
st.shardColl(collB, {_id: 1}, false);

// Split into a lot of chunks so balancing can occur
for (var i = 0; i < 10 - 1; i++) {  // 10 chunks total
    collA.getMongo().getDB("admin").runCommand({split: collA + "", middle: {_id: i}});
    collA.getMongo().getDB("admin").runCommand({split: collB + "", middle: {_id: i}});
}

// Disable balancing on one collection
sh.disableBalancing(collB);

jsTest.log("Balancing disabled on " + collB);
printjson(collA.getDB().getSisterDB("config").collections.find().toArray());

st.startBalancer();

// Make sure collA gets balanced
assert.soon(function() {
    var shardAChunks =
        st.s.getDB("config").chunks.find({_id: sh._collRE(collA), shard: shardAName}).itcount();
    var shardBChunks =
        st.s.getDB("config").chunks.find({_id: sh._collRE(collA), shard: shardBName}).itcount();
    printjson({shardA: shardAChunks, shardB: shardBChunks});
    return shardAChunks == shardBChunks;
}, "" + collA + " chunks not balanced!", 5 * 60 * 1000);

jsTest.log("Chunks for " + collA + " are balanced.");

// Check that the collB chunks were not moved
var shardAChunks =
    st.s.getDB("config").chunks.find({_id: sh._collRE(collB), shard: shardAName}).itcount();
var shardBChunks =
    st.s.getDB("config").chunks.find({_id: sh._collRE(collB), shard: shardBName}).itcount();
printjson({shardA: shardAChunks, shardB: shardBChunks});
assert(shardAChunks == 0 || shardBChunks == 0);

// Re-enable balancing for collB
sh.enableBalancing(collB);

// Make sure that collB is now balanced
assert.soon(function() {
    var shardAChunks =
        st.s.getDB("config").chunks.find({_id: sh._collRE(collB), shard: shardAName}).itcount();
    var shardBChunks =
        st.s.getDB("config").chunks.find({_id: sh._collRE(collB), shard: shardBName}).itcount();
    printjson({shardA: shardAChunks, shardB: shardBChunks});
    return shardAChunks == shardBChunks;
}, "" + collB + " chunks not balanced!", 5 * 60 * 1000);

jsTest.log("Chunks for " + collB + " are balanced.");

// Re-disable balancing for collB
sh.disableBalancing(collB);

// Wait for the balancer to fully finish the last migration and write the changelog
// MUST set db var here, ugly but necessary
db = st.s0.getDB("config");
st.awaitBalancerRound();

// Make sure auto-migrates on insert don't move chunks
var lastMigration = sh._lastMigration(collB);

var bulk = collB.initializeUnorderedBulkOp();
for (var i = 0; i < 1000000; i++) {
    bulk.insert({_id: i, hello: "world"});
}
assert.writeOK(bulk.execute());

printjson(lastMigration);
printjson(sh._lastMigration(collB));

if (lastMigration == null) {
    assert.eq(null, sh._lastMigration(collB));
} else {
    assert.eq(lastMigration.time, sh._lastMigration(collB).time);
}

st.stop();
