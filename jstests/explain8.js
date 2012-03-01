// Test calculation of the 'millis' field in explain output.

t = db.jstests_explain8;
t.drop();

t.ensureIndex( { a:1 } );
for( i = 1000; i < 4000; i += 1000 ) {
    t.save( { a:i } );
}

// Run a query with one $or clause per a-value, each of which sleeps for 'a' milliseconds.
function slow() {
    sleep( this.a );
    return true;
}
clauses = [];
for( i = 1000; i < 4000; i += 1000 ) {
    clauses.push( { a:i, $where:slow } );
}
explain = t.find( { $or:clauses } ).explain( true );
//printjson( explain );

// Verify the duration of the whole query, and of each clause.
assert.gt( explain.millis, 1000 - 500 + 2000 - 500 + 3000 - 500 );
for( j = 0; j < 3; ++j ) {
    assert.gt( explain.clauses[ j ].millis, ( j + 1 ) * 1000 - 500 );
}
