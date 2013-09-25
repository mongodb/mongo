// Test $or query with several clauses on separate indexes.

t = db.jstests_oro;
t.drop();

orClauses = [];
for( idxKey = 'a'; idxKey <= 'aaaaaaaaaa'; idxKey += 'a' ) {
    idx = {}
    idx[ idxKey ] = 1;
    t.ensureIndex( idx );
    for( i = 0; i < 200; ++i ) {
        t.insert( idx );
    }
    orClauses.push( idx );
}

printjson( t.find({$or:orClauses}).explain() );
c = t.find({$or:orClauses}).batchSize( 100 );
count = 0;

while( c.hasNext() ) {
    for( i = 0; i < 50 && c.hasNext(); ++i, c.next(), ++count );
    // Interleave with another operation.
    t.stats();
}

assert.eq( 10 * 200, count );
