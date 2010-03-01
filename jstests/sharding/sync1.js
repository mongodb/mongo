
test = new SyncCCTest( "sync1" )

t = test.conn.getDB( "test" ).sync1
t.save( { x : 1 } )
assert.eq( 1 , t.find().itcount() , "A1" );
t.save( { x : 2 } )
assert.eq( 1 , t.find().itcount() , "A2" );


test.stop();
