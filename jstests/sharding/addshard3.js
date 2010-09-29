
s = new ShardingTest( "add_shard3", 1 );

var result = s.admin.runCommand({"addshard" : "localhost:31000"});

printjson(result);

assert.eq(result.ok, 0, "don't add mongos as a shard");

