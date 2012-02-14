// Test a query with 200 $or clauses

t = db.jstests_orf;
t.drop();

a = [];
for( var i = 0; i < 200; ++i ) {
    a.push( {_id:i} );
}
a.forEach( function( x ) { t.save( x ); } );

explain = t.find( {$or:a} ).explain( true );
printjson( explain );
assert.eq( 200, explain.n );
clauses = explain.clauses;
for( i = 0; i < clauses.length; ++i ) {
    c = clauses[ i ];
    assert.eq( 'BtreeCursor _id_', c.cursor );
    assert.eq( false, c.isMultiKey );
    assert.eq( 1, c.n, 'n' );
    assert.eq( 1, c.nscannedObjects, 'nscannedObjects' );
    assert.eq( 1, c.nscanned, 'nscanned' );
    assert.eq( false, c.scanAndOrder );
    assert.eq( false, c.indexOnly );
    assert.eq( {_id:[[i,i]]}, c.indexBounds );
    allPlans = c.allPlans;
    assert.eq( 1, allPlans.length );
    plan = allPlans[ 0 ];
    assert.eq( 'BtreeCursor _id_', plan.cursor );
    assert.eq( 1, plan.n, 'n' );
    assert.eq( 1, plan.nscannedObjects, 'nscannedObjects' );
    assert.eq( 1, plan.nscanned, 'nscanned' );
    assert.eq( {_id:[[i,i]]}, plan.indexBounds );    
}
assert.eq( 200, clauses.length );
assert.eq( 200, t.count( {$or:a} ) );
