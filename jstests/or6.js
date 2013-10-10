t = db.jstests_or6;
t.drop();

t.ensureIndex( {a:1} );

// QUERY MIGRATION
// The new systems can collapse the two or clauses into one
// printjson( t.find( {$or:[{a:{$gt:2}},{a:{$gt:0}}]} ).explain() )
// printjson( t.find( {$or:[{a:{$lt:2}},{a:{$lt:4}}]} ).explain() )
// assert.eq.automsg( "2", "t.find( {$or:[{a:{$gt:2}},{a:{$gt:0}}]} ).explain().clauses[ 1 ].indexBounds.a[ 0 ][ 1 ]" );
// assert.eq.automsg( "2", "t.find( {$or:[{a:{$lt:2}},{a:{$lt:4}}]} ).explain().clauses[ 1 ].indexBounds.a[ 0 ][ 0 ]" );

// QUERY MIGRATION
// The new system does not expect the or bounds not to intersect
// printjson( t.find( {$or:[{a:{$gt:2,$lt:10}},{a:{$gt:0,$lt:5}}]} ).explain() )
// assert.eq.automsg( "2", "t.find( {$or:[{a:{$gt:2,$lt:10}},{a:{$gt:0,$lt:5}}]} ).explain().clauses[ 1 ].indexBounds.a[ 0 ][ 1 ]" );

// QUERY MIGRATION
// This was expecting the bounds to that intersect to be taken out of the second or branch
//
//printjson( t.find( {$or:[{a:{$gt:2,$lt:10}},{a:{$gt:0,$lt:15}}]} ).explain() )
//assert.eq( [ [ 0, 2 ], [ 10, 15 ] ], t.find( {$or:[{a:{$gt:2,$lt:10}},{a:{$gt:0,$lt:15}}]} ).explain().clauses[ 1 ].indexBounds.a );

// QUERY MIGRATION
// Similar bounds manipulation as above. The system expcted the or to be eliminated
// no separate clauses
// assert.eq.automsg( "null", "t.find( {$or:[{a:{$gt:2,$lt:10}},{a:{$gt:3,$lt:5}}]} ).explain().clauses" );

// QUERY MIGRATION
// See comments above
// printjson( t.find( {$or:[{a:{$gt:2,$lt:10}},{a:{$gt:3,$lt:5}},{a:{$gt:20}}]} ).explain() )
// assert.eq.automsg( "20", "t.find( {$or:[{a:{$gt:2,$lt:10}},{a:{$gt:3,$lt:5}},{a:{$gt:20}}]} ).explain().clauses[ 1 ].indexBounds.a[ 0 ][ 0 ]" );

assert.eq.automsg( "null", "t.find( {$or:[{a:1},{b:2}]} ).hint( {a:1} ).explain().clauses" );

// QUERY MIGRATION
// The current system eliminates the or
// printjson( t.find( {$or:[{a:1},{a:3}]} ).hint( {a:1} ).explain() )
// assert.eq.automsg( "2", "t.find( {$or:[{a:1},{a:3}]} ).hint( {a:1} ).explain().clauses.length" );

assert.eq.automsg( "'BasicCursor'", "t.find( {$or:[{a:1},{a:3}]} ).hint( {$natural:1} ).explain().cursor" );

// QUERY MIGRATION
// The current system uses an or plan for this
// assert.eq( null, t.find( {$or:[{a:{$gt:1,$lt:5},b:6}, {a:3,b:{$gt:0,$lt:10}}]} ).explain().clauses );

t.ensureIndex( {b:1} );
assert.eq.automsg( "2", "t.find( {$or:[{a:1,b:5},{a:3,b:5}]} ).hint( {a:1} ).explain().clauses.length" );

t.drop();

t.ensureIndex( {a:1,b:1} );
assert.eq.automsg( "2", "t.find( {$or:[{a:{$in:[1,2]},b:5}, {a:2,b:6}]} ).explain().clauses.length" );
assert.eq.automsg( "2", "t.find( {$or:[{a:{$gt:1,$lte:2},b:5}, {a:2,b:6}]} ).explain().clauses.length" );
assert.eq.automsg( "2", "t.find( {$or:[{a:{$gt:1,$lte:3},b:5}, {a:2,b:6}]} ).explain().clauses.length" );
assert.eq.automsg( "null", "t.find( {$or:[{a:{$in:[1,2]}}, {a:2}]} ).explain().clauses" );

// QUERY MIGRATION
// The current system choses and $or plan for this query
// printjson( t.find( {$or:[{a:{$gt:1,$lt:5},b:{$gt:0,$lt:3},c:6}, {a:3,b:{$gt:1,$lt:2},c:{$gt:0,$lt:10}}]} ).explain() )
// assert.eq( null, t.find( {$or:[{a:{$gt:1,$lt:5},b:{$gt:0,$lt:3},c:6}, {a:3,b:{$gt:1,$lt:2},c:{$gt:0,$lt:10}}]} ).explain().clauses );

// QUERY MIGRATION
// The current system choses and $or plan for this query
// printjson( t.find( {$or:[{a:{$gt:1,$lt:5},c:6}, {a:3,b:{$gt:1,$lt:2},c:{$gt:0,$lt:10}}]} ).explain() )
// assert.eq( null, t.find( {$or:[{a:{$gt:1,$lt:5},c:6}, {a:3,b:{$gt:1,$lt:2},c:{$gt:0,$lt:10}}]} ).explain().clauses );

// QUERY MIGRATION
// See comments above. This is optimizing the bounds of the second or.
exp = t.find( {$or:[{a:{$gt:1,$lt:5},b:{$gt:0,$lt:3},c:6}, {a:3,b:{$gt:1,$lt:4},c:{$gt:0,$lt:10}}]} ).explain();
// printjson( exp )
// assert.eq( 3, exp.clauses[ 1 ].indexBounds.b[ 0 ][ 0 ] );
assert.eq( 4, exp.clauses[ 1 ].indexBounds.b[ 0 ][ 1 ] );
