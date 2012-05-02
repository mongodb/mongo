// check that we don't crash if an index used by an earlier or clause is dropped

// Dropping an index kills all cursors on the indexed namespace, not just those 
// cursors using the dropped index.  This test is to serve as a reminder that
// the $or implementation may need minor adjustments (memory ownership) if this
// behavior is changed.

t = db.jstests_ord;
t.drop();

t.ensureIndex( {a:1} );
t.ensureIndex( {b:1} );

for( i = 0; i < 80; ++i ) {
    t.save( {a:1} );
}

for( i = 0; i < 100; ++i ) {
    t.save( {b:1} );
}

c = t.find( { $or: [ {a:1}, {b:1} ] } ).batchSize( 100 );
for( i = 0; i < 90; ++i ) {
    c.next();
}
// At this point, our initial query has ended and there is a client cursor waiting
// to read additional documents from index {b:1}.  Deduping is performed against
// the index key {a:1}.

t.dropIndex( {a:1} );
db.getLastError();

// Dropping an index kills all cursors on the indexed namespace, not just those 
// cursors using the dropped index.
assert.throws( c.next() );
