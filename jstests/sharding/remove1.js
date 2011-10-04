s = new ShardingTest( "remove_shard1", 2 );

assert.eq( 2, s.config.shards.count() , "initial server count wrong" );

s.config.databases.insert({_id: 'local', partitioned: false, primary: 'shard0000'});

// first remove puts in draining mode, the second actually removes
assert( s.admin.runCommand( { removeshard: "shard0000" } ).ok , "failed to start draining shard" );
assert( !s.admin.runCommand( { removeshard: "shard0001" } ).ok , "allowed two draining shards" );
assert( s.admin.runCommand( { removeshard: "shard0000" } ).ok , "failed to remove shard" );
assert.eq( 1, s.config.shards.count() , "removed server still appears in count" );

assert( !s.admin.runCommand( { removeshard: "shard0001" } ).ok , "allowed removing last shard" );

assert.isnull( s.config.databases.findOne({_id: 'local'}), 'should have removed local db');

// should create a shard0002 shard
conn = startMongodTest( 29000 );
assert( s.admin.runCommand( { addshard: "localhost:29000" } ).ok, "failed to add shard" );
assert.eq( 2, s.config.shards.count(), "new server does not appear in count" );

stopMongod( 29000 );
s.stop();
