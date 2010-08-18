// shard3.js

s = new ShardingTest( "shard3" , 2 , 1 , 2 );

s2 = s._mongos[1];

s.adminCommand( { enablesharding : "test" } );
s.adminCommand( { shardcollection : "test.foo" , key : { num : 1 } } );

s.config.databases.find().forEach( printjson )

a = s.getDB( "test" ).foo;
b = s2.getDB( "test" ).foo;

primary = s.getServer( "test" ).getDB( "test" ).foo;
secondary = s.getOther( primary.name ).getDB( "test" ).foo;

a.save( { num : 1 } );
a.save( { num : 2 } );
a.save( { num : 3 } );

assert.eq( 3 , a.find().toArray().length , "normal A" );
assert.eq( 3 , b.find().toArray().length , "other A" );

assert.eq( 3 , primary.count() , "p1" )
assert.eq( 0 , secondary.count() , "s1" )

assert.eq( 1 , s.onNumShards( "foo" ) , "on 1 shards" );

s.adminCommand( { split : "test.foo" , middle : { num : 2 } } );
s.adminCommand( { movechunk : "test.foo" , find : { num : 3 } , to : s.getOther( s.getServer( "test" ) ).name } );

assert( primary.find().toArray().length > 0 , "blah 1" );
assert( secondary.find().toArray().length > 0 , "blah 2" );
assert.eq( 3 , primary.find().itcount() + secondary.find().itcount() , "blah 3" )

assert.eq( 3 , a.find().toArray().length , "normal B" );
assert.eq( 3 , b.find().toArray().length , "other B" );

printjson( primary._db._adminCommand( "shardingState" ) );

// --- filtering ---

function doCounts( name , total ){
    total = total || ( primary.count() + secondary.count() );
    assert.eq( total , a.count() , name + " count" );    
    assert.eq( total , a.find().sort( { n : 1 } ).itcount() , name + " itcount - sort n" );
    assert.eq( total , a.find().itcount() , name + " itcount" );
    assert.eq( total , a.find().sort( { _id : 1 } ).itcount() , name + " itcount - sort _id" );
    return total;
}

var total = doCounts( "before wrong save" )
//secondary.save( { num : -3 } );
//doCounts( "after wrong save" , total )

// --- move all to 1 ---
print( "MOVE ALL TO 1" );

assert.eq( 2 , s.onNumShards( "foo" ) , "on 2 shards" );
s.printCollectionInfo( "test.foo" );

assert( a.findOne( { num : 1 } ) )
assert( b.findOne( { num : 1 } ) )

print( "GOING TO MOVE" );
assert( a.findOne( { num : 1 } ) , "pre move 1" )
s.printCollectionInfo( "test.foo" );
myto = s.getOther( s.getServer( "test" ) ).name
print( "counts before move: " + tojson( s.shardCounts( "foo" ) ) );
s.adminCommand( { movechunk : "test.foo" , find : { num : 1 } , to : myto } )
print( "counts after move: " + tojson( s.shardCounts( "foo" ) ) );
s.printCollectionInfo( "test.foo" );
assert.eq( 1 , s.onNumShards( "foo" ) , "on 1 shard again" );
assert( a.findOne( { num : 1 } ) , "post move 1" )
assert( b.findOne( { num : 1 } ) , "post move 2" )

print( "*** drop" );

s.printCollectionInfo( "test.foo" , "before drop" );
a.drop();
s.printCollectionInfo( "test.foo" , "after drop" );

assert.eq( 0 , a.count() , "a count after drop" )
assert.eq( 0 , b.count() , "b count after drop" )

s.printCollectionInfo( "test.foo" , "after counts" );

assert.eq( 0 , primary.count() , "p count after drop" )
assert.eq( 0 , secondary.count() , "s count after drop" )

primary.save( { num : 1 } );
secondary.save( { num : 4 } );

assert.eq( 1 , primary.count() , "p count after drop adn save" )
assert.eq( 1 , secondary.count() , "s count after drop save " )


print("*** makes sure that sharding knows where things live" );

assert.eq( 1 , a.count() , "a count after drop and save" )
s.printCollectionInfo( "test.foo" , "after a count" );
assert.eq( 1 , b.count() , "b count after drop and save" )
s.printCollectionInfo( "test.foo" , "after b count" );

assert( a.findOne( { num : 1 } ) , "a drop1" );
assert.isnull( a.findOne( { num : 4 } ) , "a drop1" );

s.printCollectionInfo( "test.foo" , "after a findOne tests" );

assert( b.findOne( { num : 1 } ) , "b drop1" );
assert.isnull( b.findOne( { num : 4 } ) , "b drop1" );

s.printCollectionInfo( "test.foo" , "after b findOne tests" );

print( "*** dropDatabase setup" )

s.printShardingStatus()
s.adminCommand( { shardcollection : "test.foo" , key : { num : 1 } } );
a.save( { num : 2 } );
a.save( { num : 3 } );
s.adminCommand( { split : "test.foo" , middle : { num : 2 } } );
s.adminCommand( { movechunk : "test.foo" , find : { num : 3 } , to : s.getOther( s.getServer( "test" ) ).name } );
s.printShardingStatus();

s.printCollectionInfo( "test.foo" , "after dropDatabase setup" );
doCounts( "after dropDatabase setup2" )
s.printCollectionInfo( "test.foo" , "after dropDatabase setup3" );

print( "*** ready to call dropDatabase" )
res = s.getDB( "test" ).dropDatabase();
assert.eq( 1 , res.ok , "dropDatabase failed : " + tojson( res ) );

s.printShardingStatus();
s.printCollectionInfo( "test.foo" , "after dropDatabase call 1" );
assert.eq( 0 , doCounts( "after dropDatabase called" ) )

// ---- retry commands SERVER-1471 ----

s.adminCommand( { enablesharding : "test2" } );
s.adminCommand( { shardcollection : "test2.foo" , key : { num : 1 } } );
dba = s.getDB( "test2" );
dbb = s2.getDB( "test2" );
dba.foo.save( { num : 1 } );
dba.foo.save( { num : 2 } );
dba.foo.save( { num : 3 } );
dba.getLastError();

assert.eq( 1 , s.onNumShards( "foo" , "test2" ) , "B on 1 shards" );
assert.eq( 3 , dba.foo.count() , "Ba" );
assert.eq( 3 , dbb.foo.count() , "Bb" );

s.adminCommand( { split : "test2.foo" , middle : { num : 2 } } );
s.adminCommand( { movechunk : "test2.foo" , find : { num : 3 } , to : s.getOther( s.getServer( "test2" ) ).name } );

assert.eq( 2 , s.onNumShards( "foo" , "test2" ) , "B on 2 shards" );

x = dba.foo.stats()
printjson( x )
y = dbb.foo.stats()
printjson( y )




s.stop();
