// sync2.js

s = new ShardingTest( "sync2" , 3 , 50 , 2 , { sync : true } );

s2 = s._mongos[1];

s.adminCommand( { enablesharding : "test" } );
s.adminCommand( { shardcollection : "test.foo" , key : { num : 1 } } );

s.getDB( "test" ).foo.insert( { num : 1 } );
s.getDB( "test" ).foo.insert( { num : 2 } );
s.getDB( "test" ).foo.insert( { num : 3 } );
s.getDB( "test" ).foo.insert( { num : 4 } );
s.getDB( "test" ).foo.insert( { num : 5 } );
s.getDB( "test" ).foo.insert( { num : 6 } );
s.getDB( "test" ).foo.insert( { num : 7 } );

assert.eq( 7 , s.getDB( "test" ).foo.find().toArray().length , "normal A" );
assert.eq( 7 , s2.getDB( "test" ).foo.find().toArray().length , "other A" );

s.adminCommand( { split : "test.foo" , middle : { num : 4 } } );
s.adminCommand( { movechunk : "test.foo" , find : { num : 3 } , to : s.getFirstOther( s.getServer( "test" ) ).name } );

assert( s._connections[0].getDB( "test" ).foo.find().toArray().length > 0 , "blah 1" );
assert( s._connections[1].getDB( "test" ).foo.find().toArray().length > 0 , "blah 2" );
assert.eq( 7 , s._connections[0].getDB( "test" ).foo.find().toArray().length + 
           s._connections[1].getDB( "test" ).foo.find().toArray().length  , "blah 3" );

assert.eq( 7 , s.getDB( "test" ).foo.find().toArray().length , "normal B" );
assert.eq( 7 , s2.getDB( "test" ).foo.find().toArray().length , "other B" );

s.adminCommand( { split : "test.foo" , middle : { num : 2 } } );
s.printChunks();

print( "* A" );

assert.eq( 7 , s.getDB( "test" ).foo.find().toArray().length , "normal B 1" );
assert.eq( 7 , s2.getDB( "test" ).foo.find().toArray().length , "other B 2" );
print( "* B" );
assert.eq( 7 , s.getDB( "test" ).foo.find().toArray().length , "normal B 3" );
assert.eq( 7 , s2.getDB( "test" ).foo.find().toArray().length , "other B 4" );

for ( var i=0; i<10; i++ ){
    print( "* C " + i );
    assert.eq( 7 , s2.getDB( "test" ).foo.find().toArray().length , "other B " + i );
}

hashes = []

for ( i=0; i<3; i++ ){
    print( i );
    s._connections[i].getDB( "config" ).chunks.find( {} , { lastmod : 1 } ).forEach( printjsononeline );
    hashes[i] = s._connections[i].getDB( "config" ).runCommand( "dbhash" );
}

printjson( hashes );

for ( i=1; i<hashes.length; i++ ){
    assert.eq( hashes[0].md5  , hashes[i].md5 , "hashes different" );
}

s.stop();
