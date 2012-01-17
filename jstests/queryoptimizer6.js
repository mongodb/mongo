// Test that $ne constraints are accounted for in QueryPattern. SERVER-4665

t = db.jstests_queryoptimizer6;

function reset() {
    t.drop();
    t.save( {a:1} );
    t.ensureIndex( {b:1}, {sparse:true} );
}

reset();
// The sparse index will be used, and recorded for this query pattern.
assert.eq( 0, t.find( {a:1,b:{$ne:1}} ).itcount() );
// The query pattern should be different, and the sparse index should not be used.
assert.eq( 1, t.find( {a:1} ).itcount() );

reset();
// The sparse index will be used, and (for better or worse) recorded for this query pattern.
assert.eq( 0, t.find( {a:1} ).min({b:1}).itcount() );
// The sparse index should not be used, even though the query patterns match.
assert.eq( 1, t.find( {a:1} ).itcount() );

reset();
t.ensureIndex( {a:1,b:1} );
// The sparse index will be used, and (for better or worse) recorded for this query pattern.
assert.eq( 0, t.find( {a:1,b:null} ).min({b:1}).itcount() );
// Descriptive test - the recorded {b:1} index is used, because it is not useless.
assert.eq( 0, t.find( {a:1,b:null} ).itcount() );
