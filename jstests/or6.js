t = db.jstests_or6;
t.drop();

t.ensureIndex( {a:1} );

assert.eq.automsg( "2", "t.find( {$or:[{a:{$gt:2}},{a:{$gt:0}}]} ).explain().clauses[ 1 ].indexBounds.a[ 0 ][ 1 ]" );
assert.eq.automsg( "2", "t.find( {$or:[{a:{$lt:2}},{a:{$lt:4}}]} ).explain().clauses[ 1 ].indexBounds.a[ 0 ][ 0 ]" );

assert.eq.automsg( "2", "t.find( {$or:[{a:{$gt:2,$lt:10}},{a:{$gt:0,$lt:5}}]} ).explain().clauses[ 1 ].indexBounds.a[ 0 ][ 1 ]" );

assert.eq( [ [ 0, 2 ], [ 10, 15 ] ], t.find( {$or:[{a:{$gt:2,$lt:10}},{a:{$gt:0,$lt:15}}]} ).explain().clauses[ 1 ].indexBounds.a );

// no separate clauses
assert.eq.automsg( "null", "t.find( {$or:[{a:{$gt:2,$lt:10}},{a:{$gt:3,$lt:5}}]} ).explain().clauses" );

assert.eq.automsg( "20", "t.find( {$or:[{a:{$gt:2,$lt:10}},{a:{$gt:3,$lt:5}},{a:{$gt:20}}]} ).explain().clauses[ 1 ].indexBounds.a[ 0 ][ 0 ]" );

assert.eq.automsg( "null", "t.find( {$or:[{a:1},{b:2}]} ).hint( {a:1} ).explain().clauses" );
assert.eq.automsg( "2", "t.find( {$or:[{a:1},{a:3}]} ).hint( {a:1} ).explain().clauses.length" );
assert.eq.automsg( "'BasicCursor'", "t.find( {$or:[{a:1},{a:3}]} ).hint( {$natural:1} ).explain().cursor" );

assert.eq( null, t.find( {$or:[{a:{$gt:1,$lt:5},b:6}, {a:3,b:{$gt:0,$lt:10}}]} ).explain().clauses );

t.ensureIndex( {b:1} );
assert.eq.automsg( "2", "t.find( {$or:[{a:1,b:5},{a:3,b:5}]} ).hint( {a:1} ).explain().clauses.length" );

t.drop();

t.ensureIndex( {a:1,b:1} );
assert.eq.automsg( "2", "t.find( {$or:[{a:{$in:[1,2]},b:5}, {a:2,b:6}]} ).explain().clauses.length" );
assert.eq.automsg( "2", "t.find( {$or:[{a:{$gt:1,$lte:2},b:5}, {a:2,b:6}]} ).explain().clauses.length" );
assert.eq.automsg( "2", "t.find( {$or:[{a:{$gt:1,$lte:3},b:5}, {a:2,b:6}]} ).explain().clauses.length" );
assert.eq.automsg( "null", "t.find( {$or:[{a:{$in:[1,2]}}, {a:2}]} ).explain().clauses" );

assert.eq( null, t.find( {$or:[{a:{$gt:1,$lt:5},b:{$gt:0,$lt:3},c:6}, {a:3,b:{$gt:1,$lt:2},c:{$gt:0,$lt:10}}]} ).explain().clauses );
assert.eq( null, t.find( {$or:[{a:{$gt:1,$lt:5},c:6}, {a:3,b:{$gt:1,$lt:2},c:{$gt:0,$lt:10}}]} ).explain().clauses );
exp = t.find( {$or:[{a:{$gt:1,$lt:5},b:{$gt:0,$lt:3},c:6}, {a:3,b:{$gt:1,$lt:4},c:{$gt:0,$lt:10}}]} ).explain();
assert.eq( 3, exp.clauses[ 1 ].indexBounds.b[ 0 ][ 0 ] );
assert.eq( 4, exp.clauses[ 1 ].indexBounds.b[ 0 ][ 1 ] );
