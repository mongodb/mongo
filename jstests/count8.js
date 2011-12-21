// Test count yielding, in both fast and normal count modes.

t = db.jstests_count8;
t.drop();

function checkYield( dropCollection, fastCount, query ) {

    obj = fastCount ? {a:true} : {a:1};
    query = query || obj;
    
    passed = false;
    for( nDocs = 20000; nDocs < 2000000; nDocs *= 2 ) {

        t.drop();
        t.ensureIndex( {a:1} );
        for( i = 0; i < nDocs; ++i ) {
            t.insert( obj );
        }
        db.getLastError();

        if ( dropCollection ) {
            p = startParallelShell( 'sleep( 30 ); db.jstests_count8.drop(); db.getLastError();' );
        } else {
            p = startParallelShell( 'sleep( 30 ); db.jstests_count8.update( {$atomic:true}, {$set:{a:-1}}, false, true ); db.getLastError();' );
        }

        printjson( query );
        count = t.count( query );
        // We test that count yields by requesting a concurrent operation modifying the collection
        // and checking that the count result is modified.
        print( 'count: ' + count + ', nDocs: ' + nDocs );
        if ( count < nDocs ) {
            passed = true;
            p();
            break;
        }
    
        p();
    }

    assert( passed );
}

checkYield( true, false );
checkYield( false, false );
checkYield( true, true );
checkYield( false, true );
checkYield( true, false, {$or:[{a:1},{a:2}]} );
