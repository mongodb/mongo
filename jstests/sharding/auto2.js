// auto2.js

s = new ShardingTest( "auto2" , 2 , 1 , 2 );

s.adminCommand( { enablesharding : "test" } );
s.adminCommand( { shardcollection : "test.foo" , key : { num : 1 } } );

bigString = "";
while ( bigString.length < 1024 * 50 )
    bigString += "asocsancdnsjfnsdnfsjdhfasdfasdfasdfnsadofnsadlkfnsaldknfsad";

db = s.getDB( "test" )
coll = db.foo;

var i=0;

for ( j=0; j<30; j++ ){
    print( "j:" + j + " : " + 
           Date.timeFunc( 
               function(){
                   for ( var k=0; k<100; k++ ){
                       coll.save( { num : i , s : bigString } );
                       i++;
                   }
               } 
           ) );
    
}
assert.eq( i , j * 100 , "setup" );
// Until SERVER-9715 is fixed, the sync command must be run on a diff connection
new Mongo( s.s.host ).adminCommand( "connpoolsync" );
db.getLastError();

print( "done inserting data" );

print( "datasize: " + tojson( s.getServer( "test" ).getDB( "admin" ).runCommand( { datasize : "test.foo" } ) ) );
s.printChunks();

function doCountsGlobal(){
    counta = s._connections[0].getDB( "test" ).foo.count(); 
    countb = s._connections[1].getDB( "test" ).foo.count(); 
    return counta + countb;
}

// Wait for the chunks to distribute
assert.soon( function(){
    doCountsGlobal()

    print( "Counts: " + counta + countb)
    
    return counta > 0 && countb > 0
})


print( "checkpoint B" )

var missing = [];

for ( i=0; i<j*100; i++ ){
    var x = coll.findOne( { num : i } );
    if ( ! x ){
        missing.push( i );
        print( "can't find: " + i );
        sleep( 5000 );
        x = coll.findOne( { num : i } );
        if ( ! x ){
            print( "still can't find: " + i );
            
            for ( var zzz=0; zzz<s._connections.length; zzz++ ){
                if ( s._connections[zzz].getDB( "test" ).foo.findOne( { num : i } ) ){
                    print( "found on wrong server: " + s._connections[zzz] );
                }
            }
            
        }
    }
}



s.printChangeLog();

print( "missing: " + tojson( missing ) )
assert.soon( function(z){ return doCountsGlobal() == j * 100; } , "from each a:" + counta + " b:" + countb + " i:" + i );
print( "checkpoint B.a" )
s.printChunks();
assert.eq( j * 100 , coll.find().limit(100000000).itcount() , "itcount A" );
assert.eq( j * 100 , counta + countb , "from each 2 a:" + counta + " b:" + countb + " i:" + i );
assert( missing.length == 0 , "missing : " + tojson( missing ) );

print( "checkpoint C" )

assert( Array.unique( s.config.chunks.find().toArray().map( function(z){ return z.shard; } ) ).length == 2 , "should be using both servers" );

for ( i=0; i<100; i++ ){
    cursor = coll.find().batchSize(5);
    cursor.next();
    cursor = null;
    gc(); 
}

print( "checkpoint D")

// test not-sharded cursors
db = s.getDB( "test2" ); 
t = db.foobar;
for ( i =0; i<100; i++ )
    t.save( { _id : i } );
for ( i=0; i<100; i++ ){
    t.find().batchSize( 2 ).next();
    assert.lt( 0 , db.runCommand( "cursorInfo" ).totalOpen , "cursor1" );
    gc();
}

for ( i=0; i<100; i++ ){
    gc();
}
assert.eq( 0 , db.runCommand( "cursorInfo" ).totalOpen , "cursor2" );

// Stop the balancer, otherwise it may grab some connections from the pool for itself
s.stopBalancer()

print( "checkpoint E")

x = db.runCommand( "connPoolStats" );
printjson( x )
for ( host in x.hosts ){
    
    // Ignore all non-shard connections in this check for used sharded
    // connections, only check those with 0 timeout.
    if (!/.*::0$/.test(host)) continue;

    // Connection pooling may change in the near future
    // TODO: Refactor / remove this test to make sure it stays relevant
    var foo = x.hosts[host];
    assert.lt( 0 , foo.available , "pool: " + host );
}

print( "checkpoint F")

assert( t.findOne() , "check close 0" );

for ( i=0; i<20; i++ ){
    temp = new Mongo( db.getMongo().host )
    temp2 = temp.getDB( "test2" ).foobar;
    assert.eq( temp._fullNameSpace , t._fullNameSpace , "check close 1" );
    assert( temp2.findOne() , "check close 2" );
    temp = null;
    gc();
}

print( "checkpoint G")

assert.throws( function(){ s.getDB( "test" ).foo.find().sort( { s : 1 } ).forEach( function( x ){ printjsononeline( x.substring( 0, x.length > 30 ? 30 : x.length ) ) } ) } )

print( "checkpoint H")

s.stop();
