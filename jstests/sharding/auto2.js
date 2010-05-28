// auto2.js

s = new ShardingTest( "auto2" , 2 , 1 , 1 );

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
s.adminCommand( "connpoolsync" );
db.getLastError();

print( "done inserting data" );

print( "datasize: " + tojson( s.getServer( "test" ).getDB( "admin" ).runCommand( { datasize : "test.foo" } ) ) );
s.printChunks();

counta = s._connections[0].getDB( "test" ).foo.count(); 
countb = s._connections[1].getDB( "test" ).foo.count(); 

assert( counta > 0 , "diff1" );
assert( countb > 0 , "diff2" );

print( "checkpoint B" )

assert.eq( j * 100 , counta + countb , "from each a:" + counta + " b:" + countb + " i:" + i );
print( "checkpoint B.a" )
assert.eq( j * 100 , coll.find().limit(100000000).itcount() , "itcount A" );

print( "checkpoint C" )

assert( Array.unique( s.config.chunks.find().toArray().map( function(z){ return z.shard; } ) ).length == 2 , "should be using both servers" );

for ( i=0; i<100; i++ ){
    cursor = coll.find().batchSize(5);
    cursor.next();
    cursor = null;
    gc(); 
}


// test not-sharded cursors
db = s.getDB( "test2" ); 
t = db.foobar;
for ( i =0; i<100; i++ )
    t.save( { _id : i } );
for ( i=0; i<100; i++ ){
    t.find().batchSize( 2 ).next();
    assert.lt( 0 , db.runCommand( "cursorInfo" ).total , "cursor1" );
    gc();
}

gc(); gc();
assert.eq( 0 , db.runCommand( "cursorInfo" ).total , "cursor2" );

s.stop();
