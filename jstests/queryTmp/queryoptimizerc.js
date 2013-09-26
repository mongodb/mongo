// Use of an $atomic match expression does not affect choice of index.  SERVER-5354

t = db.jstests_queryoptimizerc;
t.drop();

function checkExplainResults( explain ) {
    assert.eq( 'BtreeCursor a_1', explain.cursor ); // a:1 index chosen.
    assert.eq( 1, explain.allPlans.length );        // Only one (optimal) plan is attempted.
}

t.ensureIndex( { a:1 } );

checkExplainResults( t.find( { a:1 } ).explain( true ) ); 
checkExplainResults( t.find( { a:1, $atomic:1 } ).explain( true ) ); 
