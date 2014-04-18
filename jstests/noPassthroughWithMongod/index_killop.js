// Both foreground and background index builds can be aborted using killop.  SERVER-3067

t = db.jstests_slownightly_index_killop;
t.drop();

// Insert a large number of documents, enough to ensure that an index build on these documents will
// be interrupted before complete.
for( i = 0; i < 1e6; ++i ) {
    t.save( { a:i } );
}
db.getLastError();

function debug( x ) {
//    printjson( x );
}

/** @return the op id for the running index build, or -1 if there is no current index build. */
function getIndexBuildOpId() {
    inprog = db.currentOp().inprog;
    debug( inprog );
    indexBuildOpId = -1;
    inprog.forEach( function( op ) {
                        // Identify the index build as an insert into the 'test.system.indexes'
                        // namespace.  It is assumed that no other clients are concurrently
                        // accessing the 'test' database.
                        if ( op.op == 'insert' && op.ns == 'test.system.indexes' ) {
                            debug( op.opid );
                            indexBuildOpId = op.opid;
                        }
                    } );
    return indexBuildOpId;
}

/** Test that building an index with @param 'options' can be aborted using killop. */
function testAbortIndexBuild( options ) {

    // Create an index asynchronously by using a new connection.
    new Mongo( db.getMongo().host ).getCollection( t.toString() ).createIndex( { a:1 }, options );

    // When the index build starts, find its op id.
    assert.soon( function() { return ( opId = getIndexBuildOpId() ) != -1; } );
    // Kill the index build.
    db.killOp( opId );

    // Wait for the index build to stop.
    assert.soon( function() { return getIndexBuildOpId() == -1; } );
    // Check that no new index has been created.  This verifies that the index build was aborted
    // rather than successfully completed.
    assert.eq( [ { _id:1 } ], t.getIndexKeys() );
}

testAbortIndexBuild( { background:false } );
testAbortIndexBuild( { background:true } );
