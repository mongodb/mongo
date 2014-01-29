// Test "impossible match" queries, or queries that will always have
// an empty result set.

t = db.jstests_indexn;
t.drop();

function checkImpossibleMatch( explain ) {
    printjson(explain);
    assert.eq( 0, explain.n );
    // sometimes we choose an intersection plan that results in >0 nscanned, so we check
    // nscannedObjects here.
    assert.eq( 0, explain.nscannedObjects );
    if ("indexBounds" in explain) {
        assert.eq([], explain.indexBounds.a);
    }
}

t.save( {a:1,b:[1,2]} );

t.ensureIndex( {a:1} );
t.ensureIndex( {b:1} );

// {a:1} is a single key index, so no matches are possible for this query
assert.eq( 0, t.count( {a:{$gt:5,$lt:0}} ) );
checkImpossibleMatch( t.find( {a:{$gt:5,$lt:0}} ).explain() );

assert.eq( 0, t.count( {a:{$gt:5,$lt:0},b:2} ) );
checkImpossibleMatch( t.find( {a:{$gt:5,$lt:0},b:2} ).explain() );

assert.eq( 0, t.count( {a:{$gt:5,$lt:0},b:{$gt:0,$lt:5}} ) );
checkImpossibleMatch( t.find( {a:{$gt:5,$lt:0},b:{$gt:0,$lt:5}} ).explain() );

// One clause of an $or is an "impossible match"
printjson( t.find( {$or:[{a:{$gt:5,$lt:0}},{a:1}]} ).explain() )
assert.eq( 1, t.count( {$or:[{a:{$gt:5,$lt:0}},{a:1}]} ) );
checkImpossibleMatch( t.find( {$or:[{a:{$gt:5,$lt:0}},{a:1}]} ).explain().clauses[ 0 ] );

// One clause of an $or is an "impossible match"; original order of the $or
// does not matter.
printjson( t.find( {$or:[{a:1},{a:{$gt:5,$lt:0}}]} ).explain() )
assert.eq( 1, t.count( {$or:[{a:1},{a:{$gt:5,$lt:0}}]} ) );
checkImpossibleMatch( t.find( {$or:[{a:1},{a:{$gt:5,$lt:0}}]} ).explain().clauses[ 0 ] );

t.save( {a:2} );

// Descriptive test: query system sees this query as an $or where
// one clause of the $or is an $and. The $and bounds get intersected
// forming a clause with empty index bounds. The union of the $or bounds
// produces the two point intervals [1, 1] and [2, 2].
assert.eq( 2, t.count( {$or:[{a:1},{a:{$gt:5,$lt:0}},{a:2}]} ) );
explain = t.find( {$or:[{a:1},{a:{$gt:5,$lt:0}},{a:2}]} ).explain();
printjson( explain )
assert.eq( 2, explain.clauses.length );
checkImpossibleMatch( explain.clauses[ 0 ] );
assert.eq( [[1, 1], [2,2]], explain.clauses[ 1 ].indexBounds.a );
