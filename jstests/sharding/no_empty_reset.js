// Tests that an empty shard can't be the cause of a chunk reset

var st = new ShardingTest({shards: 2, mongos: 2});

// Don't balance since we're manually moving chunks
st.stopBalancer();

var coll = st.s.getCollection(jsTestName() + ".coll");

for (var i = -10; i < 10; i++)
    coll.insert({_id: i});

st.shardColl(coll, {_id: 1}, {_id: 0});

jsTestLog("Sharded setup complete");

st.printShardingStatus();

jsTestLog("Setting initial versions for each mongos...");

coll.find().itcount();

var collB = st.s1.getCollection("" + coll);
collB.find().itcount();

jsTestLog("Migrating via first mongos...");

var fullShard = st.getShard(coll, {_id: 1});
var emptyShard = st.getShard(coll, {_id: -1});

var admin = st.s.getDB("admin");
assert.soon(
    function() {
        var result = admin.runCommand(
            {moveChunk: "" + coll, find: {_id: -1}, to: fullShard.shardName, _waitForDelete: true});
        jsTestLog('moveChunk result = ' + tojson(result));
        return result.ok;
    },
    "Setup FAILURE:  Unable to move chunk from " + emptyShard.shardName + " to " +
        fullShard.shardName);

jsTestLog("Resetting shard version via first mongos...");

coll.find().itcount();

jsTestLog("Making sure we don't insert into the wrong shard...");

collB.insert({_id: -11});

var emptyColl = emptyShard.getCollection("" + coll);

print(emptyColl);
print(emptyShard);
print(emptyShard.shardName);
st.printShardingStatus();

assert.eq(0, emptyColl.find().itcount());

jsTestLog("DONE!");
st.stop();
