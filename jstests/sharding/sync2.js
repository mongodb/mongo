// sync2.js

s = new ShardingTest( "sync2" , 3 , 50 , 2 , { sync : true } );

s.stopBalancer()

s2 = s._mongos[1];

s.adminCommand( { enablesharding : "test" } );
s.adminCommand( { shardcollection : "test.foo" , key : { num : 1 } } );

s.config.settings.update( { _id: "balancer" }, { $set : { stopped: true } } , true );

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
s.adminCommand({ movechunk: "test.foo", find: { num: 3 },
    to: s.getFirstOther(s.getServer("test" )).name, _waitForDelete: true });

assert( s._connections[0].getDB( "test" ).foo.find().toArray().length > 0 , "shard 0 request" );
assert( s._connections[1].getDB( "test" ).foo.find().toArray().length > 0 , "shard 1 request" );
assert.eq( 7 , s._connections[0].getDB( "test" ).foo.find().toArray().length + 
           s._connections[1].getDB( "test" ).foo.find().toArray().length  , "combined shards" );

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

assert.eq( 0 , s.config.big.find().itcount() , "C1" );
for ( i=0; i<50; i++ ){
    s.config.big.insert( { _id : i } );
}
s.config.getLastError();
assert.eq( 50 , s.config.big.find().itcount() , "C2" );
assert.eq( 50 , s.config.big.find().count() , "C3" );
assert.eq( 50 , s.config.big.find().batchSize(5).itcount() , "C4" );
    

hashes = []

for ( i=0; i<3; i++ ){
    print( i );
    s._connections[i].getDB( "config" ).chunks.find( {} , { lastmod : 1 } ).forEach( printjsononeline );
    hashes[i] = s._connections[i].getDB( "config" ).runCommand( "dbhash" );
}

printjson( hashes );

for ( i=1; i<hashes.length; i++ ){
    if ( hashes[0].md5 == hashes[i].md5 ) 
        continue;
    
    assert.eq( hashes[0].numCollections , hashes[i].numCollections , "num collections" );
    
    var bad = false;

    for ( var k in hashes[0].collections ){
        if ( hashes[0].collections[k] == 
             hashes[i].collections[k] )
            continue;
        
        if ( k == "mongos" || k == "changelog" || k == "locks" || k == "lockpings" )
            continue;
        
        bad = true;
        print( "collection " + k + " is different" );
        
        print( "----" );
        s._connections[0].getDB( "config" ).getCollection( k ).find().sort( { _id : 1 } ).forEach( printjsononeline );
        print( "----" );
        s._connections[i].getDB( "config" ).getCollection( k ).find().sort( { _id : 1 } ).forEach( printjsononeline );
        print( "----" );
    }

    if ( bad )
        throw "hashes different";
}

s.stop();
