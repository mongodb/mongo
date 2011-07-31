// Check null key generation.

t = db.jstests_indexv;
t.drop();

t.ensureIndex( {'a.b':1} );

t.save( {a:[{},{b:1}]} );
var e = t.find( {'a.b':null} ).explain();
assert.eq( 0, e.n );
assert.eq( 1, e.nscanned );

t.drop();
t.ensureIndex( {'a.b.c':1} );
t.save( {a:[{b:[]},{b:{c:1}}]} );
var e = t.find( {'a.b.c':null} ).explain();
assert.eq( 0, e.n );
assert.eq( 1, e.nscanned );
