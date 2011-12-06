// Test that client cannot access index namespaces SERVER-4276.

if ( 0 ) { // SERVER-4276

t = db.jstests_indexx;
t.drop();

debug = true;

idx = db.jstests_indexx.$_id_;

function shouldFail( f ) {
    e = assert.throws( function() {
                      f();
                      if( db.getLastError() ) {
                      throw db.getLastError();
                      }
                      } );
    if ( debug ) {
        printjson( e );
    }
}

function checkFailingOperations() {
    // Test that accessing the index namespace fails.
    shouldFail( function() { idx.count(); } );
    shouldFail( function() { idx.find().itcount(); } );
    shouldFail( function() { idx.insert({}); } );
    shouldFail( function() { idx.remove(); } );
    shouldFail( function() { idx.update({},{}); } );
    assert.commandFailed( idx.runCommand( 'compact' ) );
    
    // No validation here (yet).
    //shouldFail( function() { idx.ensureIndex({x:1}) } );    
}

// Check with base collection not present.
checkFailingOperations();
t.save({});
// Check with base collection present.
checkFailingOperations();

}