// Check that the explain result count does proper deduping.

t = db.jstests_explain5;
t.drop();

t.ensureIndex( {a:1} );
t.ensureIndex( {b:1} );

t.save( {a:[1,2,3],b:[4,5,6]} );
for( i = 0; i < 10; ++i ) {
    t.save( {} );
}

// Check with a single in order plan.

explain = t.find( {a:{$gt:0}} ).explain( true );
assert.eq( 1, explain.n );
assert.eq( 1, explain.allPlans[ 0 ].n );

// Check with a single out of order plan.

explain = t.find( {a:{$gt:0}} ).sort( {z:1} ).hint( {a:1} ).explain( true );
assert.eq( 1, explain.n );
assert.eq( 1, explain.allPlans[ 0 ].n );

// Check with multiple plans.

explain = t.find( {a:{$gt:0},b:{$gt:0}} ).explain( true );
assert.eq( 1, explain.n );
assert.eq( 1, explain.allPlans[ 0 ].n );
assert.eq( 1, explain.allPlans[ 1 ].n );

explain = t.find( {$or:[{a:{$gt:0},b:{$gt:0}},{a:{$gt:-1},b:{$gt:-1}}]} ).explain( true );
assert.eq( 1, explain.n );

// QUERY MIGRATION
// printjson( explain )
// "allPlan", ie the alternative plans, are not a property of each of the $or clause.
// They are a propery of the plan itself.
// So they chould be checked at 'explain.allPlans' not under clauses.
assert.eq( 1, explain.clauses[ 0 ].n );
// assert.eq( 1, explain.clauses[ 0 ].allPlans[ 0 ].n );
// assert.eq( 1, explain.clauses[ 0 ].allPlans[ 1 ].n );

// QUERY MIGRATION
// The following asserts that the second $or branch's bounds was is [-1,0] instead of [-1, MaxElem]
// This is an optimization, give that the first branch tested already for [0, MaxElem]
// assert.eq( 0, explain.clauses[ 1 ].n );
// assert.eq( 0, explain.clauses[ 1 ].allPlans[ 0 ].n );
// assert.eq( 0, explain.clauses[ 1 ].allPlans[ 1 ].n );
