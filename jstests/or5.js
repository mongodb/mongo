t = db.jstests_or5;
t.drop();

t.ensureIndex( {a:1} );
t.ensureIndex( {b:1} );

assert.eq.automsg( "'BasicCursor'", "t.find( {$or:[{a:2},{b:3},{}]} ).explain().cursor" );
assert.eq.automsg( "'BasicCursor'", "t.find( {$or:[{a:2},{b:3},{c:4}]} ).explain().cursor" );
