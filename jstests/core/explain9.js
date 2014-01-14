// Test that limit is applied by explain when there are both in order and out of order candidate
// plans.  SERVER-4150

t = db.jstests_explain9;
t.drop();

t.ensureIndex( { a:1 } );

for( i = 0; i < 10; ++i ) {
    t.save( { a:i, b:0 } );
}

explain = t.find( { a:{ $gte:0 }, b:0 } ).sort( { a:1 } ).limit( 5 ).explain( true );
// Five results are expected, matching the limit spec.
assert.eq( 5, explain.n );
explain.allPlans.forEach( function( x ) {
                         // Five results are expected for the in order plan.
                         if ( x.cursor == "BtreeCursor a_1" ) {
                             assert.eq( 5, x.n );
                         }
                         else {
                             assert.gte( 5, x.n );
                         }
                         } );
