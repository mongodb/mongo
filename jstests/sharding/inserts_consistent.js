// Test write re-routing on version mismatch.

var st = new ShardingTest({shards: 2, mongos: 2, verbose: 2});

jsTest.log("Doing test setup...");

// Stop balancer, since it'll just get in the way of this
st.stopBalancer();

var mongos = st.s;
var admin = mongos.getDB("admin");
var config = mongos.getDB("config");
var coll = st.s.getCollection(jsTest.name() + ".coll");

st.shardColl(coll, {_id: 1}, {_id: 0}, false);

jsTest.log("Refreshing second mongos...");

var mongosB = st.s1;
var adminB = mongosB.getDB("admin");
var collB = mongosB.getCollection(coll + "");

// Make sure mongosB knows about the coll
assert.eq(0, collB.find().itcount());
// printjson( adminB.runCommand({ flushRouterConfig : 1 }) )

jsTest.log("Moving chunk to create stale mongos...");

var otherShard = config.chunks.findOne({_id: sh._collRE(coll)}).shard;
for (var i = 0; i < st._shardNames.length; i++) {
    if (otherShard != st._shardNames[i]) {
        otherShard = st._shardNames[i];
        break;
    }
}

print("Other shard : " + otherShard);

printjson(admin.runCommand({moveChunk: coll + "", find: {_id: 0}, to: otherShard}));

jsTest.log("Inserting docs that needs to be retried...");

var nextId = -1;
for (var i = 0; i < 2; i++) {
    printjson("Inserting " + nextId);
    assert.writeOK(collB.insert({_id: nextId--, hello: "world"}));
}

jsTest.log("Inserting doc which successfully goes through...");

// Do second write
assert.writeOK(collB.insert({_id: nextId--, goodbye: "world"}));

// Assert that write went through
assert.eq(coll.find().itcount(), 3);

jsTest.log("Now try moving the actual chunk we're writing to...");

// Now move the actual chunk we're writing to
printjson(admin.runCommand({moveChunk: coll + "", find: {_id: -1}, to: otherShard}));

jsTest.log("Inserting second docs to get written back...");

// Will fail entirely if too many of these, waiting for write to get applied can get too long.
for (var i = 0; i < 2; i++) {
    collB.insert({_id: nextId--, hello: "world"});
}

// Refresh server
printjson(adminB.runCommand({flushRouterConfig: 1}));

jsTest.log("Inserting second doc which successfully goes through...");

// Do second write
assert.writeOK(collB.insert({_id: nextId--, goodbye: "world"}));

jsTest.log("All docs written this time!");

// Assert that writes went through.
assert.eq(coll.find().itcount(), 6);

jsTest.log("DONE");

st.stop();
