/**
* this tests some of the ground work
*/

s = new ShardingTest( "shard1" , 2 );

db = s.getDB( "test" );
db.foo.insert( { num : 1 , name : "eliot" } );
db.foo.insert( { num : 2 , name : "sara" } );
db.foo.insert( { num : -1 , name : "joe" } );
db.foo.ensureIndex( { num : 1 } );
assert.eq( 3 , db.foo.find().length() , "A" );

shardCommand = { shardcollection : "test.foo" , key : { num : 1 } };

assert.throws( function(){ s.adminCommand( shardCommand ); } );

s.adminCommand( { enablesharding : "test" } );
assert.eq( 3 , db.foo.find().length() , "after partitioning count failed" );

s.adminCommand( shardCommand );

cconfig = s.config.collections.findOne( { _id : "test.foo" } );
assert( cconfig , "why no collection entry for test.foo" )
delete cconfig.lastmod
delete cconfig.dropped
assert.eq( cconfig , { _id : "test.foo" , key : { num : 1 } , unique : false } , "Sharded content" );

s.config.collections.find().forEach( printjson )

assert.eq( 1 , s.config.chunks.count() , "num chunks A");
si = s.config.chunks.findOne();
assert( si );
assert.eq( si.ns , "test.foo" );

assert.eq( 3 , db.foo.find().length() , "after sharding, no split count failed" );

// SERVER-4284, test modified because of SERVER-5020
var invalidDB = s.getDB( "foobar" );
// hack to bypass invalid database name checking at the DB constructor
invalidDB._name = "foo bar";
invalidDB.blah.insert( { x : 1 } );
assert.isnull( s.config.databases.findOne( { _id : "foo bar" } ) );


s.stop();
