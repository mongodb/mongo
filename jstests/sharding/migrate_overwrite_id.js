//
// Tests that a migration does not overwrite duplicate _ids on data transfer
//

var st = new ShardingTest({shards: 2, mongos: 1});
st.stopBalancer();

var mongos = st.s0;
var admin = mongos.getDB("admin");
var coll = mongos.getCollection("foo.bar");

assert(admin.runCommand({enableSharding: coll.getDB() + ""}).ok);
printjson(admin.runCommand({movePrimary: coll.getDB() + "", to: st.shard0.shardName}));
assert(admin.runCommand({shardCollection: coll + "", key: {skey: 1}}).ok);
assert(admin.runCommand({split: coll + "", middle: {skey: 0}}).ok);
assert(admin.runCommand({moveChunk: coll + "", find: {skey: 0}, to: st.shard1.shardName}).ok);

var id = 12345;

jsTest.log("Inserting a document with id : 12345 into both shards with diff shard key...");

assert.writeOK(coll.insert({_id: id, skey: -1}));
assert.writeOK(coll.insert({_id: id, skey: 1}));

printjson(st.shard0.getCollection(coll + "").find({_id: id}).toArray());
printjson(st.shard1.getCollection(coll + "").find({_id: id}).toArray());
assert.eq(2, coll.find({_id: id}).itcount());

jsTest.log("Moving both chunks to same shard...");

var result = admin.runCommand({moveChunk: coll + "", find: {skey: -1}, to: st.shard1.shardName});
printjson(result);

printjson(st.shard0.getCollection(coll + "").find({_id: id}).toArray());
printjson(st.shard1.getCollection(coll + "").find({_id: id}).toArray());
assert.eq(2, coll.find({_id: id}).itcount());

st.stop();
