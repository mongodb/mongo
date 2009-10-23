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

for( i = 20; i < 40; ++i ) {
    t.save( { i : i } );
}
assert( !t.findOne( { i : 19 } ) );
assert( !c.hasNext() );
assert( !d.hasNext() );
assert( t.find().sort( { i : 1 } ).hint( { i : 1 } ).toArray().length > 10 );

assert( t.findOne( { i : 38 } ) );
t.remove( { i : 38 } );
assert( db.getLastError().indexOf( "capped" ) >= 0 );

assert( t.validate().valid );
