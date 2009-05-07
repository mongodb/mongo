t = db.jstests_norepeat;

t.drop();
t.ensureIndex( { i: 1 } );
for( i = 0; i < 3; ++i ) {
    t.save( { i: i } );
}

c = t.find().hint( { i: 1 } ).limit( 2 );
assert.eq( 0, c.next().i );
t.update( { i: 0 }, { i: 3 } );
assert.eq( 1, c.next().i );
assert.eq( 2, c.next().i );
assert( !c.hasNext(), "unexpected: object found" );
