// When an index becomes multikey the query plan cache is cleared.  SERVER-5301

t = db.jstests_queryoptimizer9;
t.drop();

function recordQueryPlan() {
    t.find( { a:1, b:1 } ).explain();
}

function hasCachedPlan() {
    return !!t.find( { a:1, b:1 } ).explain( true ).oldPlan;
}

t.ensureIndex( { a:1 } );
t.save( { a:1 } );

recordQueryPlan();
assert( hasCachedPlan() );

// While the index is not multikey the cached query plan is not cleared.
t.save( { a:2 } );
assert( hasCachedPlan() );

// When the index becomes multikey the cached query plan is cleared.
t.save( { a:[ 3, 4 ] } );
assert( !hasCachedPlan() );

// When the index remains multikey the cached query plan is not cleared.
recordQueryPlan();
t.save( { a:[ 5, 6 ] } );
assert( hasCachedPlan() );
