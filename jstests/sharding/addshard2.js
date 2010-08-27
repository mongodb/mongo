
// don't start any shards, yet
s = new ShardingTest( "add_shard2", 1, 0, 1, {useHostname : true} );

var conn1 = startMongodTest( 30001 , "add_shard21" , 0 , {useHostname : true} );
var conn2 = startMongodTest( 30002 , "add_shard22" , 0 , {useHostname : true} );

var rs1 = new ReplSetTest( { "name" : "add_shard2_rs1", nodes : 3 , startPort : 31200 } );
rs1.startSet();
rs1.initiate();
var master1 = rs1.getMaster();

var rs2 = new ReplSetTest( { "name" : "add_shard2_rs2", nodes : 3 , startPort : 31203 } );
rs2.startSet();
rs2.initiate();
var master2 = rs2.getMaster();

// name given
assert(s.admin.runCommand({"addshard" : getHostName()+":30001", "name" : "bar"}).ok, "failed to add shard");
var shard = s.getDB("config").shards.findOne({"_id" : {"$nin" : ["shard0000"]}});
assert(shard, "shard wasn't found");
assert.eq("bar", shard._id, "shard has incorrect name");

// replica set
assert(s.admin.runCommand({"addshard" : "add_shard2_rs1/"+getHostName()+":31200"}).ok, "failed to add shard");
shard = s.getDB("config").shards.findOne({"_id" : {"$nin" : ["shard0000", "bar"]}});
assert(shard, "shard wasn't found");
assert.eq("add_shard2_rs1", shard._id, "t2 name");

// replica set w/ name given
assert(s.admin.runCommand({"addshard" : "add_shard2_rs2/"+getHostName()+":31200", "name" : "myshard"}).ok, "failed to add shard");
shard = s.getDB("config").shards.findOne({"_id" : {"$nin" : ["shard0000", "bar", "add_shard2_rs1"]}});
assert(shard, "shard wasn't found");
assert.eq("myshard", shard._id, "t3 name");

// no name given
assert(s.admin.runCommand({"addshard" : getHostName()+":30002"}).ok, "failed to add shard");
shard = s.getDB("config").shards.findOne({"_id" : {"$nin" : ["shard0000", "bar", "add_shard2_rs1", "myshard"]}});
assert(shard, "shard wasn't found");
assert.eq("shard0001", shard._id, "t4 name");

assert.eq(s.getDB("config").shards.count(), 5, "unexpected number of shards");

