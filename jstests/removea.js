// Test removal of a substantial proportion of inserted documents.  SERVER-3803
// A complete test will only be performed against a DEBUG build.

t = db.jstests_removea;

Random.setRandomSeed();

for( v = 0; v < 2; ++v ) { // Try each index version.
    t.drop();
    t.ensureIndex( { a:1 }, { v:v } );
    for( i = 0; i < 10000; ++i ) {
        t.save( { a:i } );
    }
    
    toDrop = [];
    for( i = 0; i < 10000; ++i ) {
        toDrop.push( Random.randInt( 10000 ) ); // Dups in the query will be ignored.
    }
    // Remove many of the documents; $atomic prevents use of a ClientCursor, which would invoke a
    // different bucket deallocation procedure than the one to be tested (see SERVER-4575).
    t.remove( { a:{ $in:toDrop }, $atomic:true } );
    assert( !db.getLastError() );
}
