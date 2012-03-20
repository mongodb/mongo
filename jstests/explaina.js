// Check explain results when an in order plan is selected among mixed in order and out of order
// plans.

t = db.jstests_explaina;
t.drop();

t.ensureIndex( { a:1 } );
t.ensureIndex( { b:1 } );

for( i = 0; i < 1000; ++i ) {
    t.save( { a:i, b:i%3 } );
}

// Query with an initial set of documents.
explain1 = t.find( { a:{ $gte:0 }, b:2 } ).sort( { a:1 } ).explain( true );

for( i = 1000; i < 2000; ++i ) {
    t.save( { a:i, b:i%3 } );
}

// Query with some additional documents.
explain2 = t.find( { a:{ $gte:0 }, b:2 } ).sort( { a:1 } ).explain( true );

function plan( explain, cursor ) {
    for( i in explain.allPlans ) {
        e = explain.allPlans[ i ];
        if ( e.cursor == cursor ) {
            return e;
        }
    }
    assert( false );
}

// Check query totals.
assert.eq( 333, explain1.n );
assert.eq( 666, explain2.n );

// Check totals for the selected in order a:1 plan.
assert.eq( 333, plan( explain1, "BtreeCursor a_1" ).n );
assert.eq( 1000, plan( explain1, "BtreeCursor a_1" ).nscanned );
assert.eq( 666, plan( explain2, "BtreeCursor a_1" ).n );
assert.eq( 2000, plan( explain2, "BtreeCursor a_1" ).nscanned );

// Check that results only examined after the a:1 plan is selected will not affect plan explain
// output for other plans.
assert.eq( plan( explain1, "BtreeCursor b_1" ), plan( explain2, "BtreeCursor b_1" ) );
assert.eq( plan( explain1, "BasicCursor" ), plan( explain2, "BasicCursor" ) );
