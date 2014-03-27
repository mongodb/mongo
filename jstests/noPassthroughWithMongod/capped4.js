t = db.jstests_capped4;
t.drop();

db.createCollection( "jstests_capped4", {size:1000,capped:true} );
t.ensureIndex( { i: 1 } );
for( i = 0; i < 20; ++i ) {
    t.save( { i : i } );
}
c = t.find().sort( { $natural: -1 } ).limit( 2 );
c.next();
c.next();
d = t.find().sort( { i: -1 } ).limit( 2 );
d.next();
d.next();

for( i = 20; t.findOne( { i:19 } ); ++i ) {
    t.save( { i : i } );
}
//assert( !t.findOne( { i : 19 } ), "A" );
assert( !c.hasNext(), "B" );
assert( !d.hasNext(), "C" );
assert( t.find().sort( { i : 1 } ).hint( { i : 1 } ).toArray().length > 10, "D" );

assert( t.findOne( { i : i - 1 } ), "E" );
t.remove( { i : i - 1 } );
assert( db.getLastError().indexOf( "capped" ) >= 0, "F" );

assert( t.validate().valid, "G" );

/* there is a cursor open here, so this is a convenient place for a quick cursor test. */

db._adminCommand("closeAllDatabases");

assert( db.serverStatus().cursors.totalOpen == 0, "cursors open and shouldn't be");
