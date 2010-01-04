db.fsync2.drop();

d = db.getSisterDB( "admin" );

assert.commandWorked( d.runCommand( {fsync:1, lock: 1 } ) );

// uncomment when fixed SERVER-519
// db.fsync2.save( {x:1 } );

m = new Mongo( db.getMongo().host );
print( "sending unlock command" );
assert.commandWorked( m.getDB( "admin" ).runCommand( {unlock:1} ) );

// uncomment when fixed SERVER-519
//assert.eq( 1, db.fsync2.count() );
