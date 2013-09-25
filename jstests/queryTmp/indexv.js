// Check null key generation.

t = db.jstests_indexv;
t.drop();

t.ensureIndex( {'a.b':1} );

t.save( {a:[{},{b:1}]} );
// NEW QUERY EXPLAIN
var count = t.find( {'a.b':null} ).itcount();
// NEW QUERY EXPLAIN
assert.eq( 1, count );
/* NEW QUERY EXPLAIN
assert.eq( 1, e.nscanned );
*/

t.drop();
t.ensureIndex( {'a.b.c':1} );
t.save( {a:[{b:[]},{b:{c:1}}]} );
// NEW QUERY EXPLAIN
var count = t.find( {'a.b.c':null} ).itcount();
// NEW QUERY EXPLAN
assert.eq( 0, count );
/* NEW QUERY EXPLAIN
assert.eq( 1, e.nscanned );
*/
