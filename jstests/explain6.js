// Test explain result count when a skip parameter is used.

t = db.jstests_explain6;
t.drop();

t.save( {} );
explain = t.find().skip( 1 ).explain( true );
assert.eq( 0, explain.n );
// With only one plan, the skip information is known for the plan.  This is an arbitrary
// implementation detail, but it changes the way n is calculated.
assert.eq( 0, explain.allPlans[ 0 ].n );

t.ensureIndex( {a:1} );
explain = t.find( {a:null,b:null} ).skip( 1 ).explain( true );
assert.eq( 0, explain.n );

// QUERY MIGRATION
// printjson( explain )
// Old Comment: With multiple plans, the skip information is not known to the plan
// In the new query system, the skip applies to alternative plans as well
// assert.eq( 1, explain.allPlans[ 0 ].n );

t.dropIndexes();
explain = t.find().skip( 1 ).sort({a:1}).explain( true );
// Skip is applied for an in memory sort.
assert.eq( 0, explain.n );
printjson(explain);
// See above comment about query migration
// assert.eq( 1, explain.allPlans[ 0 ].n );
