s = new ShardingTest( "remove_shard1", 2 );

assert.eq( 2, s.config.shards.count() , "initial server count wrong" );

// first remove puts in draining mode, second remove
assert( s.admin.runCommand( { removeshard: "shard0" } ).ok , "failed to start draining shard" );
assert( s.admin.runCommand( { removeshard: "shard0" } ).ok , "failed to remove shard" );
assert.eq( 1, s.config.shards.count() , "removed server still appears in count" );

// to fix in SERVER-1418
// assert( s.admin.runCommand( { addshard: "127.0.0.1:43415", allowLocal : true } ).ok, "failed to add shard" );
// assert.eq( 2, s.config.shards.count(), "new server does not appear in count" );

s.stop()