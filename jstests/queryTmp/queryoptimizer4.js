// Check that a new plan will be selected if a recorded plan performs poorly.

t = db.jstests_queryoptimizer4;

function reset( matches, filler ) {
    t.drop();
    
    t.ensureIndex({a:1});
    t.ensureIndex({b:1});
    
    // {a:1} index best for query {a:1,b:1}
    t.save({a:1,b:1});
    t.save({a:2,b:1});
    
    // {b:1} index best for query {a:100,b:100}
    for( i = 0; i < matches; ++i ) {
        t.save({a:100,b:100});        
    }
    for( i = 0; i < filler; ++i ) {
        t.save({a:100,b:5});
    }
}

function checkCursor( query, cursor ) {
    t.find(query).itcount();
    // Check that index on 'cursor' was chosen in the above query.
    var x = t.find(query).explain(true);
    assert.eq( 'BtreeCursor ' + cursor, x.oldPlan.cursor , tojson(x) );    
}

// Check {b:1} takes over when {a:1} is much worse for query {a:100,b:100}.
reset( 1, 50 );
checkCursor( {a:1,b:1}, 'a_1' );
checkCursor( {a:100,b:100}, 'b_1' );

// Check smallest filler for which {b:1} will take over.
reset( 2, 12 );
checkCursor( {a:1,b:1}, 'a_1' );
checkCursor( {a:100,b:100}, 'b_1' );

// Check largest filler for which {b:1} will not take over - demonstrating that new plans run alongside the old plan.
reset( 2, 11 );
checkCursor( {a:1,b:1}, 'a_1' );
checkCursor( {a:100,b:100}, 'a_1' );
