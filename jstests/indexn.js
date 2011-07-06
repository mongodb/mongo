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

assert.eq( 0, t.count( {a:{$gt:5,$lt:0}} ) );
// {a:1} is a single key index, so no matches are possible for this query
checkImpossibleMatchDummyCursor( t.find( {a:{$gt:5,$lt:0}} ).explain() );

assert.eq( 0, t.count( {a:{$gt:5,$lt:0},b:2} ) );
checkImpossibleMatchDummyCursor( t.find( {a:{$gt:5,$lt:0},b:2} ).explain() );

assert.eq( 0, t.count( {a:{$gt:5,$lt:0},b:{$gt:0,$lt:5}} ) );
checkImpossibleMatchDummyCursor( t.find( {a:{$gt:5,$lt:0},b:{$gt:0,$lt:5}} ).explain() );

assert.eq( 1, t.count( {$or:[{a:{$gt:5,$lt:0}},{a:1}]} ) );
checkImpossibleMatchDummyCursor( t.find( {$or:[{a:{$gt:5,$lt:0}},{a:1}]} ).explain().clauses[ 0 ] );

// A following invalid range is eliminated.
assert.eq( 1, t.count( {$or:[{a:1},{a:{$gt:5,$lt:0}}]} ) );
assert.eq( null, t.find( {$or:[{a:1},{a:{$gt:5,$lt:0}}]} ).explain().clauses );

t.save( {a:2} );

// An intermediate invalid range is eliminated.
assert.eq( 2, t.count( {$or:[{a:1},{a:{$gt:5,$lt:0}},{a:2}]} ) );
explain = t.find( {$or:[{a:1},{a:{$gt:5,$lt:0}},{a:2}]} ).explain();
assert.eq( 2, explain.clauses.length );
assert.eq( [[1,1]], explain.clauses[ 0 ].indexBounds.a );
assert.eq( [[2,2]], explain.clauses[ 1 ].indexBounds.a );
