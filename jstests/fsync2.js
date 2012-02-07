if (!jsTest.options().auth) { // SERVER-4243

db.fsync2.drop();

db.fsync2.save( {x:1} );

d = db.getSisterDB( "admin" );

assert.commandWorked( d.runCommand( {fsync:1, lock: 1 } ) );

for ( i=0; i<200; i++) {
    assert.eq(1, db.fsync2.count());
    sleep(1);
}

db.fsync2.save( {x:1} );

m = new Mongo( db.getMongo().host );

// Uncomment once SERVER-4243 is fixed
//assert.eq(1, m.getDB(db.getName()).fsync2.count());

assert( m.getDB("admin").$cmd.sys.unlock.findOne().ok );

db.getLastError();

assert.eq( 2, db.fsync2.count() );

}
