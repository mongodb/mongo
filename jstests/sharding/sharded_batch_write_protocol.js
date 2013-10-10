//
// Tests batch writes in a sharded cluster
//

var options = {separateConfig : true};

var st = new ShardingTest({shards : 1, mongos : 1, other : options});
st.stopBalancer();

var mongos = st.s0;
var shards = mongos.getDB("config").shards.find().toArray();
var admin = mongos.getDB("admin");
var coll = mongos.getCollection("foo.bar");

assert(admin.runCommand({enableSharding : coll.getDB() + ""}).ok);
printjson(admin.runCommand({movePrimary : coll.getDB() + "", to : shards[0]._id}));
assert(admin.runCommand({shardCollection : coll + "", key : {_id : 1}}).ok);

var oid = new ObjectId();
request = {insert : coll + "", documents : [{_id : oid}], writeConcern : {}, continueOnError : true};

printjson(coll.getDB().runCommand(request));
assert.eq(coll.findOne()._id, oid);
