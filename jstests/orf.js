// Test a query with 200 $or clauses

t = db.jstests_orf;
t.drop();

a = [];
for( var i = 0; i < 200; ++i ) {
    a.push( {_id:i} );
}
a.forEach( function( x ) { t.save( x ); } );

explain = t.find( {$or:a} ).explain();
assert.eq( 200, explain.n );
assert.eq( 200, explain.clauses.length );
assert.eq( 200, t.count( {$or:a} ) );
