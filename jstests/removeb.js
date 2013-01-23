// Test removal of Records that have been reused since the remove operation began.  SERVER-5198

t = db.jstests_removeb;
t.drop();

t.ensureIndex( { a:1 } );

// Make the index multikey to trigger cursor dedup checking.
t.insert( { a:[ -1, -2 ] } );
t.remove();

// Insert some data.
for( i = 0; i < 20000; ++i ) {
    t.insert( { a:i } );
}
db.getLastError();

p = startParallelShell(
                       // Wait until the remove operation (below) begins running.
                       'while( db.jstests_removeb.count() == 20000 );' +
                       // Insert documents with increasing 'a' values.  These inserted documents may
                       // reuse Records freed by the remove operation in progress and will be
                       // visited by the remove operation if it has not completed.
                       'for( i = 20000; i < 40000; ++i ) {' +
                       '    db.jstests_removeb.insert( { a:i } );' +
                       '    db.getLastError();' +
                       '    if (i % 1000 == 0) {' +
                       '        print( i-20000 + \" of 20000 documents inserted\" );' +
                       '    }' +
                       '}'
                       );

// Remove using the a:1 index in ascending direction.
t.remove( { a:{ $gte:0 } } );
assert( !db.getLastError(), 'The remove operation failed.' );

p();

t.drop();
