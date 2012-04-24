// Sanity test for removing documents with adjacent index keys.  SERVER-2008

t = db.jstests_removec;
t.drop();
t.ensureIndex( { a:1 } );

/** @return an array containing a sequence of numbers from i to i + 10. */
function runStartingWith( i ) {
    ret = [];
    for( j = 0; j < 11; ++j ) {
        ret.push( i + j );
    }
    return ret;
}

// Insert some documents with adjacent index keys.
for( i = 0; i < 1100; i += 11 ) {
    t.save( { a:runStartingWith( i ) } );
}
db.getLastError();

// Remove and then reinsert random documents in the background.
s = startParallelShell(
                       't = db.jstests_removec;' +
                       'for( j = 0; j < 1000; ++j ) {' +
                       '    o = t.findOne( { a:Random.randInt( 1100 ) } );' +
                       '    t.remove( { _id:o._id } );' +
                       '    t.insert( o );' +
                       '}'
                       );

// Find operations are error free.
for( i = 0; i < 200; ++i ) {
    t.find( { a:{ $gte:0 } } ).hint( { a:1 } ).itcount();
    assert( !db.getLastError() );
}

s();

t.drop();
