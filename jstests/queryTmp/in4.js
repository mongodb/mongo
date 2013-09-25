t = db.jstests_in4;

function checkRanges( a, b ) {
    assert.eq( a, b );
}

t.drop();
t.ensureIndex( {a:1,b:1} );
// NEW QUERY EXPLAIN
assert.eq( 0, t.find( {a:2,b:3} ).itcount() );
// NEW QUERY EXPLAIN
assert.eq( 0, t.find( {a:{$in:[2,3]},b:4} ).itcount() );
// NEW QUERY EXPLAIN
assert.eq( 0, t.find( {a:2,b:{$in:[3,4]}} ).itcount() );
// NEW QUERY EXPLAIN
assert.eq( 0, t.find( {a:{$in:[2,3]},b:{$in:[4,5]}} ).itcount() );

// NEW QUERY EXPLAIN
assert.eq( 0, t.find( {a:{$in:[2,3]},b:{$gt:4,$lt:10}} ).itcount() );

t.save( {a:1,b:1} );
t.save( {a:2,b:4.5} );
t.save( {a:2,b:4} );
// NEW QUERY EXPLAIN
assert.eq( 1, t.find( {a:{$in:[2,3]},b:{$in:[4,5]}} ).hint( {a:1,b:1} ).itcount() );
// NEW QUERY EXPLAIN
assert.eq( 4, t.findOne( {a:{$in:[2,3]},b:{$in:[4,5]}} ).b );

t.drop();
t.ensureIndex( {a:1,b:1,c:1} );
// NEW QUERY EXPLAIN
assert.eq( 0, t.find( {a:2,b:{$in:[3,4]},c:5} ).itcount() );

t.save( {a:2,b:3,c:5} );
t.save( {a:2,b:3,c:4} );
// NEW QUERY EXPLAIN
assert.eq( 1, t.find( {a:2,b:{$in:[3,4]},c:5} ).hint( {a:1,b:1,c:1} ).itcount() );
t.remove();
t.save( {a:2,b:4,c:5} );
t.save( {a:2,b:4,c:4} );
// NEW QUERY EXPLAIN
assert.eq( 1, t.find( {a:2,b:{$in:[3,4]},c:5} ).hint( {a:1,b:1,c:1} ).itcount() );

t.drop();
t.ensureIndex( {a:1,b:-1} );
// NEW QUERY EXPLAIN
e = t.find( {a:2,b:{$in:[3,4]}} ).explain();
/* NEW QUERY EXPLAIN
checkRanges( {a:[[2,2]],b:[[4,4],[3,3]]}, ib );
*/
/* NEW QUERY EXPLAIN
assert( ib.b[ 0 ][ 0 ] > ib.b[ 1 ][ 0 ] );
*/
e = t.find( {a:2,b:{$in:[3,4]}} ).sort( {a:-1,b:1} ).explain();
/* NEW QUERY EXPLAIN
checkRanges( {a:[[2,2]],b:[[3,3],[4,4]]}, ib );
*/
/* NEW QUERY EXPLAIN
assert( ib.b[ 0 ][ 0 ] < ib.b[ 1 ][ 0 ] );
*/
