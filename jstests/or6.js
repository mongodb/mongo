t = db.jstests_or6;
t.drop();

t.ensureIndex( {a:1} );

assert.eq.automsg( "null", "t.find( {$or:[{a:1},{b:2}]} ).hint( {a:1} ).explain().clauses" );

assert.eq.automsg( "'BasicCursor'", "t.find( {$or:[{a:1},{a:3}]} ).hint( {$natural:1} ).explain().cursor" );

t.ensureIndex( {b:1} );
assert.eq.automsg( "2", "t.find( {$or:[{a:1,b:5},{a:3,b:5}]} ).hint( {a:1} ).explain().clauses.length" );

t.drop();

t.ensureIndex( {a:1,b:1} );
assert.eq.automsg( "2", "t.find( {$or:[{a:{$in:[1,2]},b:5}, {a:2,b:6}]} )" +
                        ".hint({a:1,b:1}).explain().clauses.length" );
assert.eq.automsg( "2", "t.find( {$or:[{a:{$gt:1,$lte:2},b:5}, {a:2,b:6}]} )" +
                        ".hint({a:1,b:1}).explain().clauses.length" );
assert.eq.automsg( "2", "t.find( {$or:[{a:{$gt:1,$lte:3},b:5}, {a:2,b:6}]} )" +
                        ".hint({a:1,b:1}).explain().clauses.length" );
assert.eq.automsg( "null", "t.find( {$or:[{a:{$in:[1,2]}}, {a:2}]} )" +
                        ".hint({a:1,b:1}).explain().clauses" );
