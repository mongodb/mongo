// check that we don't scan $ne values

t = db.jstests_ne2;
t.drop();
t.ensureIndex( {a:1} );

t.save( { a:-0.5 } );
t.save( { a:0 } );
t.save( { a:0 } );
t.save( { a:0.5 } );

// NEW QUERY EXPLAIN
assert.eq(t.find( { a: { $ne: 0 } } ).itcount(), 2);
/* NEW QUERY EXPLAIN
assert.eq( "BtreeCursor a_1 multi", e.cursor );
*/
/* NEW QUERY EXPLAIN
assert.eq( 0, e.indexBounds.a[ 0 ][ 1 ] );
*/
/* NEW QUERY EXPLAIN
assert.eq( 0, e.indexBounds.a[ 1 ][ 0 ] );
*/
/* NEW QUERY EXPLAIN
assert.eq( 3, e.nscanned );
*/

// NEW QUERY EXPLAIN
assert.eq(t.find( { a: { $gt: -1, $lt: 1, $ne: 0 } } ).itcount(), 2);
/* NEW QUERY EXPLAIN
assert.eq( "BtreeCursor a_1 multi", e.cursor );
*/
/* NEW QUERY EXPLAIN
assert.eq( { a: [ [ -1, 0 ], [ 0, 1 ] ] }, e.indexBounds );
*/
/* NEW QUERY EXPLAIN
assert.eq( 3, e.nscanned );
*/
