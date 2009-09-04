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

print( "done inserting data" );

print( "datasize: " + tojson( s.getServer( "test" ).getDB( "admin" ).runCommand( { datasize : "test.foo" } ) ) );
s.printChunks();

counta = s._connections[0].getDB( "test" ).foo.count(); 
countb = s._connections[1].getDB( "test" ).foo.count(); 

assert.eq( j * 100 , counta + countb , "from each a:" + counta + " b:" + countb + " i:" + i );
assert.eq( j * 100 , coll.find().limit(100000000).itcount() , "itcount A" );

assert( Array.unique( s.config.chunks.find().toArray().map( function(z){ return z.shard; } ) ).length == 2 , "should be using both servers" );

s.stop();
