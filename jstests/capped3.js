t = db.jstests_capped3;
t2 = db.jstests_capped3_clone;
t.drop();
t2.drop();
for( i = 0; i < 1000; ++i ) {
    t.save( {i:i} );
}
assert.commandWorked( db.runCommand( { cloneCollectionAsCapped:"jstests_capped3", toCollection:"jstests_capped3_clone", size:100000 } ) );
c = t2.find();
for( i = 0; i < 1000; ++i ) {
    assert.eq( i, c.next().i );
}
assert( !c.hasNext() );

t.drop();
t2.drop();

for( i = 0; i < 1000; ++i ) {
    t.save( {i:i} );
}
assert.commandWorked( db.runCommand( { cloneCollectionAsCapped:"jstests_capped3", toCollection:"jstests_capped3_clone", size:1000 } ) );
c = t2.find().sort( {$natural:-1} );
i = 999;
while( c.hasNext() ) {
    assert.eq( i--, c.next().i );
}
assert( i < 990 );

t.drop();
t2.drop();

for( i = 0; i < 1000; ++i ) {
    t.save( {i:i} );
}
assert.commandWorked( t.convertToCapped( 1000 ) );
c = t.find().sort( {$natural:-1} );
i = 999;
while( c.hasNext() ) {
    assert.eq( i--, c.next().i );
}
assert( i < 990 );
assert( i > 900 );
