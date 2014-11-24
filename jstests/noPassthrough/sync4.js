
test = new SyncCCTest( "sync4" )

db = test.conn.getDB( "test" )
t = db.sync4

for ( i=0; i<1000; i++ ){
    t.insert( { _id : i , x : "asdasdsdasdas" } )
}
db.getLastError();

test.checkHashes( "test" , "A0" );
assert.eq( 1000 , t.find().count() , "A1" )
assert.eq( 1000 , t.find().itcount() , "A2" )
assert.eq( 1000 , t.find().snapshot().batchSize(10).itcount() , "A2" )



test.stop();
