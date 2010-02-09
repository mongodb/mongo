// Test background index creation w/ constraints

parallel = function() {
    return db[ baseName + "_parallelStatus" ];
}

resetParallel = function() {
    parallel().drop();
}

doParallel = function( work ) {
    resetParallel();
    startMongoProgramNoConnect( "mongo", "--eval", work + "; db." + baseName + "_parallelStatus.save( {done:1} );", db.getMongo().host );
}

doneParallel = function() {
    return !!parallel().findOne();
}

waitParallel = function() {
    assert.soon( function() { return doneParallel(); }, "parallel did not finish in time", 300000, 1000 );
}

doTest = function( dropDups ) {

size = 10000;
while( 1 ) { // if indexing finishes before we can run checks, try indexing w/ more data
    print( "size: " + size );
    baseName = "jstests_index12";
    fullName = "db." + baseName;
    t = db[ baseName ];
    t.drop();

    db.eval( function( size ) {
                for( i = 0; i < size; ++i ) {
                    db.jstests_index12.save( {i:i} );
                }
            },
            size );
    assert.eq( size, t.count() );
    
    doParallel( fullName + ".ensureIndex( {i:1}, {background:true, unique:true, dropDups:" + dropDups + "} )" );
    try {
        // wait for indexing to start
        assert.soon( function() { return 2 == db.system.indexes.count( {ns:"test."+baseName} ) }, "no index created", 30000, 50 );
        t.save( {i:0} );
        printjson( db.getLastError() );
        t.save( {i:size-1} );
        printjson( db.getLastError() );
    } catch( e ) {
        // only a failure if we're still indexing
        // wait for parallel status to update to reflect indexing status
        sleep( 1000 );
        if ( !doneParallel() ) {
            throw e;
        }
    }
    if ( !doneParallel() ) {
        break;
    }
    print( "indexing finished too soon, retrying..." );
    size *= 2;
    assert( size < 5000000, "unable to run checks in parallel with index creation" );
}

waitParallel();
assert.eq( size, t.find().toArray().length, "full query failed" );
assert.eq( size, t.count(), "count failed" );
    
}

// SERVER-619
//doTest( "false" );
//doTest( "true" );