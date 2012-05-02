// Test background index creation

load( "jstests/libs/slow_weekly_util.js" )

testServer = new SlowWeeklyMongod( "indexbg1" )
db = testServer.getDB( "test" );

parallel = function() {
    return db[ baseName + "_parallelStatus" ];
}

resetParallel = function() {
    parallel().drop();
}

doParallel = function(work) {
    resetParallel();
    print("doParallel: " + work);
    startMongoProgramNoConnect("mongo", "--eval", work + "; db." + baseName + "_parallelStatus.save( {done:1} );", db.getMongo().host);
}

doneParallel = function() {
    return !!parallel().findOne();
}

waitParallel = function() {
    assert.soon( function() { return doneParallel(); }, "parallel did not finish in time", 300000, 1000 );
}

// waiting on SERVER-620

print( "index11.js host:" );
print( db.getMongo().host );

size = 500000;
while( 1 ) { // if indexing finishes before we can run checks, try indexing w/ more data
    print( "size: " + size );
    baseName = "jstests_index11";
    fullName = "db." + baseName;
    t = db[ baseName ];
    t.drop();

    for( i = 0; i < size; ++i ) {
        db.jstests_index11.save( {i:i} );
    }
    db.getLastError();
    assert.eq( size, t.count() );
    
    doParallel( fullName + ".ensureIndex( {i:1}, {background:true} )" );
    try {
        // wait for indexing to start
        print("wait for indexing to start");
        assert.soon( function() { return 2 == db.system.indexes.count( {ns:"test."+baseName} ) }, "no index created", 30000, 50 );
        print("started.");
        assert.eq( size, t.count() );
        assert.eq( 100, t.findOne( {i:100} ).i );
        q = t.find();
        for( i = 0; i < 120; ++i ) { // getmore
            q.next();
            assert( q.hasNext(), "no next" );
        }
        var ex = t.find( {i:100} ).limit(-1).explain()
        printjson(ex)
        assert.eq( "BasicCursor", ex.cursor, "used btree cursor" );
        assert( ex.nscanned < 1000 , "took too long to find 100: " + tojson( ex ) );
        t.remove( {i:40} );
        t.update( {i:10}, {i:-10} );
        id = t.find().hint( {$natural:-1} ).next()._id;
        t.update( {_id:id}, {i:-2} );
        t.save( {i:-50} );
        t.save( {i:size+2} );
        assert( !db.getLastError() );

        print("calling ensureIndex");
        t.ensureIndex( {i:1} );

        printjson( db.getLastError() );
        assert( db.getLastError() );
        assert.eq( size + 1, t.count() );
        assert( !db.getLastError() );

        print("calling dropIndex");
        t.dropIndex( {i:1} );
        printjson( db.getLastError() );
        assert( db.getLastError() );        
    } catch( e ) {
        // only a failure if we're still indexing
        // wait for parallel status to update to reflect indexing status
        print("caught exception: " + e );
        sleep( 1000 );
        if ( !doneParallel() ) {
            throw e;
        }
        print("but that's OK")
    }
    if ( !doneParallel() ) {
        break;
    }
    print( "indexing finished too soon, retrying..." );
    size *= 2;
    assert( size < 20000000, "unable to run checks in parallel with index creation" );
}

print("our tests done, waiting for parallel to finish");
waitParallel();
print("finished");

assert.eq( "BtreeCursor i_1", t.find( {i:100} ).explain().cursor );
assert.eq( 1, t.count( {i:-10} ) );
assert.eq( 1, t.count( {i:-2} ) );
assert.eq( 1, t.count( {i:-50} ) );
assert.eq( 1, t.count( {i:size+2} ) );
assert.eq( 0, t.count( {i:40} ) );
assert( !db.getLastError() );
print("about to drop index");
t.dropIndex( {i:1} );
printjson( db.getLastError() );
assert( !db.getLastError() );

testServer.stop();
