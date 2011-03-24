// SERVER-2831 Demonstration of sparse index matching semantics in a multi index $or query.

t = db.jstests_orh;
t.drop();

t.ensureIndex( {a:1}, {sparse:true} );
t.ensureIndex( {b:1,a:1} );

t.remove();
t.save( {b:2} );
assert.eq( 0, t.count( {a:null} ) );
assert.eq( 1, t.count( {b:2,a:null} ) );

assert.eq( 1, t.count( {$or:[{b:2,a:null},{a:null}]} ) );

// Is this desired?
assert.eq( 0, t.count( {$or:[{a:null},{b:2,a:null}]} ) );
