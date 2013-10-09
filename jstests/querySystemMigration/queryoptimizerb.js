// The query optimizer will not generate 'special' plans inside $or clauses.  SERVER-5936

t = db.jstests_queryoptimizerb;
t.drop();

t.ensureIndex( { a:'2d' } );
// Add an index to make the { b:1 } query mongo::HELPFUL but QueryPlan::Unhelpful.
t.ensureIndex( { c:1, b:1 } );

// A 'special' index is not used for an $or equality match query.
t.save( { a:[ 0, 0 ], b:1 } );
assert.eq( 1, t.find( { a:[ 0, 0 ], $or:[ { b:1 } ] } ).itcount() );
assert.eq( 'BasicCursor', t.find( { a:[ 0, 0 ], $or:[ { b:1 } ] } ).explain().cursor );

// A 'special' index cannot be hinted for a $or query.
assert.throws( function() {
               t.find( { a:[ 0, 0 ], $or:[ { a:[ 0, 0 ] } ] } ).hint( { a:'2d' } ).itcount(); } );
assert.throws( function() {
               t.find( { a:[ 0, 0 ], $or:[ { a:[ 0, 0 ] } ] } ).hint( { a:'2d' } ).explain(); } );
