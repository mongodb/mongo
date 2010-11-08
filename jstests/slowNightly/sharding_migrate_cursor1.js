
chunksize = 25

s = new ShardingTest( "migrate_cursor1" , 2 , 1 , 1 , { chunksize : chunksize } );

s.config.settings.update( { _id: "balancer" }, { $set : { stopped: true } } , true );

s.adminCommand( { enablesharding : "test" } );
db = s.getDB( "test" )
t = db.foo

bigString = ""
stringSize = 1024;

while ( bigString.length < stringSize )
    bigString += "asdasdas";

stringSize = bigString.length
docsPerChunk = Math.ceil( ( chunksize * 1024 * 1024 ) / ( stringSize - 12 ) )
numChunks = 5
numDocs = 20 * docsPerChunk

print( "stringSize: " + stringSize + " docsPerChunk: " + docsPerChunk + " numDocs: " + numDocs )

for ( i=0; i<numDocs; i++ ){
    t.insert( { _id : i , s : bigString } );
}

db.getLastError();

s.adminCommand( { shardcollection : "test.foo" , key : { _id : 1 } } );

assert.lt( numChunks ,  s.config.chunks.find().count() , "initial 1" );

primary = s.getServer( "test" ).getDB( "test" ).foo;
secondaryName = s.getOther( primary.name )
secondary = secondaryName.getDB( "test" ).foo;

assert.eq( numDocs , primary.count() , "initial 2" );
assert.eq( 0 , secondary.count() , "initial 3" );
assert.eq( numDocs , t.count() , "initial 4" )

x = primary.find( { _id : { $lt : 500 } } ).batchSize(2)
x.next();

s.adminCommand( { moveChunk : "test.foo" , find : { _id : 0 } , to : secondaryName.name } )

join = startParallelShell( "sleep(5); db.x.insert( {x:1} ); db.adminCommand( { moveChunk : 'test.foo' , find : { _id : " + docsPerChunk * 3 + " } , to : '" + secondaryName.name + "' } )" )
assert.soon( function(){ return db.x.count() > 0; } , "XXX" , 30000 , 1 )


print( "itcount: " + x.itcount() )
x = null;
for ( i=0; i<5; i++ ) gc()

print( "cursor should be gone" )

join();

//assert.soon( function(){ return numDocs == t.count(); } , "at end 1" )
sleep( 5000 )
assert.eq( numDocs , t.count() , "at end 2" )
assert.eq( numDocs , primary.count() + secondary.count() , "at end 3" )

s.stop()
