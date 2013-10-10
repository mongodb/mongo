// Check fast detection of empty result set with a single key index SERVER-958.

t = db.jstests_indexn;
t.drop();

function checkImpossibleMatchDummyCursor( explain ) {
    assert.eq( 'BasicCursor', explain.cursor );
    assert.eq( 0, explain.nscanned );
    assert.eq( 0, explain.n );
}

t.save( {a:1,b:[1,2]} );

t.ensureIndex( {a:1} );
t.ensureIndex( {b:1} );

// QUERY MIGRATION
// This is assuming that a collection scan is preferable to an index scan if
// the query happens to be always empty.
// There's no point changing the plan according to the data results.
// On the other hand, tt might be interesting to have an eof runner handle these.
assert.eq( 0, t.count( {a:{$gt:5,$lt:0}} ) );
// {a:1} is a single key index, so no matches are possible for this query
// checkImpossibleMatchDummyCursor( t.find( {a:{$gt:5,$lt:0}} ).explain() );

// QUERY MIGRATION
// Same comment as above
assert.eq( 0, t.count( {a:{$gt:5,$lt:0},b:2} ) );
// checkImpossibleMatchDummyCursor( t.find( {a:{$gt:5,$lt:0},b:2} ).explain() );

// QUERY MIGRATION
// Same comment as above
assert.eq( 0, t.count( {a:{$gt:5,$lt:0},b:{$gt:0,$lt:5}} ) );
// checkImpossibleMatchDummyCursor( t.find( {a:{$gt:5,$lt:0},b:{$gt:0,$lt:5}} ).explain() );

// QUERY MIGRATION
// This case is slightly more nuanced. It is expecting a collection scan from one of the
// or branches, which is a violation of or-planning constaints.
// printjson( t.find( {$or:[{a:{$gt:5,$lt:0}},{a:1}]} ).explain() )
assert.eq( 1, t.count( {$or:[{a:{$gt:5,$lt:0}},{a:1}]} ) );
// checkImpossibleMatchDummyCursor( t.find( {$or:[{a:{$gt:5,$lt:0}},{a:1}]} ).explain().clauses[ 0 ] );

// QUERY MIGRATION
// This case goes differently. It eliminates the $or clause altoghether.
// printjson( t.find( {$or:[{a:1},{a:{$gt:5,$lt:0}}]} ).explain() )
// A following invalid range is eliminated.
assert.eq( 1, t.count( {$or:[{a:1},{a:{$gt:5,$lt:0}}]} ) );
// assert.eq( null, t.find( {$or:[{a:1},{a:{$gt:5,$lt:0}}]} ).explain().clauses );

t.save( {a:2} );

// QUERY MIGRATION
// old comment: An intermediate invalid range is eliminated.
// The new system is still picking up a collection scan for this one as it sees it
// as an $or with $and's.
assert.eq( 2, t.count( {$or:[{a:1},{a:{$gt:5,$lt:0}},{a:2}]} ) );
explain = t.find( {$or:[{a:1},{a:{$gt:5,$lt:0}},{a:2}]} ).explain();
// printjson( explain )
// assert.eq( 2, explain.clauses.length );
// assert.eq( [[1,1]], explain.clauses[ 0 ].indexBounds.a );
// assert.eq( [[2,2]], explain.clauses[ 1 ].indexBounds.a );
