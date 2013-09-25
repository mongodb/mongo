// check that we don't scan $ne values

t = db.jstests_ne2;
t.drop();
t.ensureIndex( {a:1} );

t.save( { a:-0.5 } );
t.save( { a:0 } );
t.save( { a:0 } );
t.save( { a:0.5 } );

e = t.find( { a: { $ne: 0 } } ).explain( true );
assert.eq( "BtreeCursor a_1 multi", e.cursor );
assert.eq( 0, e.indexBounds.a[ 0 ][ 1 ] );
assert.eq( 0, e.indexBounds.a[ 1 ][ 0 ] );
assert.eq( 3, e.nscanned );

e = t.find( { a: { $gt: -1, $lt: 1, $ne: 0 } } ).explain();
assert.eq( "BtreeCursor a_1 multi", e.cursor );
assert.eq( { a: [ [ -1, 0 ], [ 0, 1 ] ] }, e.indexBounds );
assert.eq( 3, e.nscanned );
