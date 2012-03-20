// Test failover from an in order plan to an out of order plan and vice versa.

t = db.jstests_queryoptimizer8;
t.drop();

t.ensureIndex( { a:1 } );
t.ensureIndex( { b:1 } );

t.save( { a:1, b:0 } );
t.save( { a:0, b:1 } );
for( i = 0; i < 1000; ++i ) {
    t.save( { a:1, b:1 } );
}

aPreferableQuery = { a:0, b:1 };
bPreferableQuery = { a:1, b:0 };

// Run an explain query to record a specified query plan for the query pattern.
function recordPlan( plan, query, sort, scanAndOrder ) {
    explain = t.find( query ).sort( sort ).explain();
    assert.eq( 'BtreeCursor ' + plan, explain.cursor );
    assert.eq( scanAndOrder, explain.scanAndOrder );
}

// Check the plan used for a query.
function checkPlanUsed( plan, query, sort ) {
    // Run the query and check its result.
    results = t.find( query ).sort( sort ).toArray();
    assert.eq( 1, results.length );
    result = results[ 0 ];
    assert.eq( query.a, result.a );
    assert.eq( query.b, result.b );
    // Check the plan used for the above query by examining explain's oldPlan field.
    assert.eq( 'BtreeCursor ' + plan, t.find( query ).sort( sort ).explain( true ).oldPlan.cursor );
}

function checkOrdered( ordered ) {
    sort = ordered ? { a:1 } : {};
    
    recordPlan( 'a_1', aPreferableQuery, sort, false );
    checkPlanUsed( 'b_1', bPreferableQuery, sort );

    recordPlan( 'b_1', bPreferableQuery, sort, ordered );
    checkPlanUsed( 'a_1', aPreferableQuery, sort );
}

checkOrdered( false );
checkOrdered( true );
