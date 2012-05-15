// Test query retry with a query that is non multikey unusable and unsatisfiable.  SERVER-5581

t = db.jstests_queryoptimizer7;
t.drop();

t.ensureIndex( { a:1 } );
t.ensureIndex( { b:1 } );

for( i = 0; i < 25; ++i ) {
    t.save( { a:0, b:'' } ); // a:0 documents have small b strings.
}
big = new Array( 1000000 ).toString();
for( i = 0; i < 50; ++i ) {
    t.save( { a:[1,3], b:big } ); // a:[1,3] documents have very large b strings.
}

// Record the a:1 index for the query pattern for { a: { $lt:1 } }, { b:1 }.
assert.eq( 'BtreeCursor a_1', t.find( { a:{ $lt:1 } } ).sort( { b:1 } ).explain().cursor );

// The multikey query pattern for this query will match that of the previous query.
// The a:1 index will be retried for this query but fail because an in memory sort must
// be performed on a larger data set.  Because the query { a:{ $lt:2, $gt:2 } } is
// unsatisfiable, no attempt will be made to clear its query pattern.
assert.lt( -1, t.find( { a:{ $lt:2, $gt:2 } } ).sort( { b:1 } ).itcount() );
