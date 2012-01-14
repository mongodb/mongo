// Test that $ne constraints are accounted for in QueryPattern. SERVER-4665

t = db.jstests_queryoptimizer6;
t.drop();

t.save( {a:1} );
t.ensureIndex( {b:1}, {sparse:true} );

// The sparse index will be used, and recorded for this query pattern.
assert.eq( 0, t.find( {a:1,b:{$ne:1}} ).itcount() );
// The query pattern should be different, and the sparse index should not be used.
assert.eq( 1, t.find( {a:1} ).itcount() );
