
test = new SyncCCTest( "sync1" )

db = test.conn.getDB( "test" )
t = db.sync1
t.save( { x : 1 } )
assert.eq( 1 , t.find().itcount() , "A1" );
assert.eq( 1 , t.find().count() , "A2" );
t.save( { x : 2 } )
assert.eq( 2 , t.find().itcount() , "A3" );
assert.eq( 2 , t.find().count() , "A4" );

test.checkHashes( "test" , "A3" );

test.tempKill();
assert.throws( function(){ t.save( { x : 3 } ); } , null , "B1" );
// It's ok even for some of the mongod to be unreachable for read-only cmd
assert.eq( 2, t.find({}).count() );
// It's NOT ok for some of the mongod to be unreachable for write cmd
assert.throws( function(){ t.getDB().runCommand({ profile: 1 }); }); 
assert.eq( 2 , t.find().itcount() , "B2" );
test.tempStart();
test.checkHashes( "test" , "B3" );

// Trying killing the second mongod
test.tempKill( 1 );
assert.throws( function(){ t.save( { x : 3 } ); } );
// It's ok even for some of the mongod to be unreachable for read-only cmd
assert.eq( 2, t.find({}).count() );
// It's NOT ok for some of the mongod to be unreachable for write cmd
assert.throws( function(){ t.getDB().runCommand({ profile: 1 }); }); 
assert.eq( 2 , t.find().itcount() );
test.tempStart( 1 );

assert.eq( 2 , t.find().itcount() , "C1" );
assert.soon( function(){
    try  {
        t.remove( { x : 1 } )
        return true;
    }
    catch ( e ){
        print( e );
    }
    return false;
} )
t.find().forEach( printjson )
assert.eq( 1 , t.find().itcount() , "C2" );

test.stop();
