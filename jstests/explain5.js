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

assert.eq( 1, explain.clauses[ 0 ].n );
assert.eq( 1, explain.clauses[ 0 ].allPlans[ 0 ].n );
assert.eq( 1, explain.clauses[ 0 ].allPlans[ 1 ].n );

assert.eq( 0, explain.clauses[ 1 ].n );
assert.eq( 0, explain.clauses[ 1 ].allPlans[ 0 ].n );
assert.eq( 0, explain.clauses[ 1 ].allPlans[ 1 ].n );
