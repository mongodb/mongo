// Test cloneCollection command

var baseName = "jstests_clonecollection";

parallel = function() {
    return t.parallelStatus;
}

resetParallel = function() {
    parallel().drop();
}

doParallel = function( work ) {
    resetParallel();
    startMongoProgramNoConnect( "mongo", "--port", ports[ 1 ], "--eval", work + "; db.parallelStatus.save( {done:1} );", baseName );
}

doneParallel = function() {
    return !!parallel().findOne();
}

waitParallel = function() {
    assert.soon( function() { return doneParallel(); }, "parallel did not finish in time", 300000, 1000 );
}

cloneNo = -1;
startstartclone = function( spec ) {
    spec = spec || "";
    cloneNo++;
    doParallel( "z = db.runCommand( {startCloneCollection:\"jstests_clonecollection.a\", from:\"localhost:" + ports[ 0 ] + "\"" + spec + " } ); print( \"clone_clone_clone_commandResult::" + cloneNo + "::\" + tojson( z , '' , true ) + \":::::\" );" );
}

finishstartclone = function() {
    waitParallel();
    // even after parallel shell finished, must wait for finishToken line to appear in log
    assert.soon( function() {
                raw = rawMongoProgramOutput().replace( /[\r\n]/gm , " " )
                ret = raw.match( new RegExp( "clone_clone_clone_commandResult::" + cloneNo + "::(.*):::::" ) );
                if ( ret == null ) {
                return false;
                }
                ret = ret[ 1 ];
                return true;
                } );
    
    eval( "ret = " + ret );
    
    assert.commandWorked( ret );
    return ret;
}

dofinishclonecmd = function( ret ) {
    finishToken = ret.finishToken;
    // Round-tripping through JS can corrupt the cursor ids we store as BSON
    // Date elements.  Date( 0 ) will correspond to a cursorId value of 0, which
    // makes the db start scanning from the beginning of the collection.
    finishToken.cursorId = new Date( 0 );    
    return t.runCommand( {finishCloneCollection:finishToken} );
}

finishclone = function( ret ) {
    assert.commandWorked( dofinishclonecmd( ret ) );    
}

ports = allocatePorts( 2 );

f = startMongod( "--port", ports[ 0 ], "--dbpath", "/data/db/" + baseName + "_from", "--nohttpinterface", "--bind_ip", "127.0.0.1" ).getDB( baseName );
t = startMongod( "--port", ports[ 1 ], "--dbpath", "/data/db/" + baseName + "_to", "--nohttpinterface", "--bind_ip", "127.0.0.1" ).getDB( baseName );

for( i = 0; i < 1000; ++i ) {
    f.a.save( { i: i } );
}
assert.eq( 1000, f.a.find().count() );

assert.commandWorked( t.cloneCollection( "localhost:" + ports[ 0 ], "a" ) );
assert.eq( 1000, t.a.find().count() );

t.a.drop();

assert.commandWorked( t.cloneCollection( "localhost:" + ports[ 0 ], "a", { i: { $gte: 10, $lt: 20 } } ) );
assert.eq( 10, t.a.find().count() );

t.a.drop();
assert.eq( 0, t.system.indexes.find().count() );

f.a.ensureIndex( { i: 1 } );
assert.eq( 2, f.system.indexes.find().count(), "expected index missing" );
assert.commandWorked( t.cloneCollection( "localhost:" + ports[ 0 ], "a" ) );
if ( t.system.indexes.find().count() != 2 ) {
    printjson( t.system.indexes.find().toArray() );
}
assert.eq( 2, t.system.indexes.find().count(), "expected index missing" );
// Verify index works
assert.eq( 50, t.a.find( { i: 50 } ).hint( { i: 1 } ).explain().startKey.i );
assert.eq( 1, t.a.find( { i: 50 } ).hint( { i: 1 } ).toArray().length, "match length did not match expected" );

// Check that capped-ness is preserved on clone
f.a.drop();
t.a.drop();

f.createCollection( "a", {capped:true,size:1000} );
assert( f.a.isCapped() );
assert.commandWorked( t.cloneCollection( "localhost:" + ports[ 0 ], "a" ) );
assert( t.a.isCapped(), "cloned collection not capped" );

// Now test insert + delete + update during clone
f.a.drop();
t.a.drop();

for( i = 0; i < 100000; ++i ) {
    f.a.save( { i: i } );
}
assert.eq( 100000, f.a.count() );

startstartclone( ", query:{i:{$gte:0}}" );

sleep( 200 );
f.a.save( { i: 200000 } );
f.a.save( { i: -1 } );
f.a.remove( { i: 0 } );
f.a.update( { i: 99998 }, { i: 99998, x: "y" } );
ret = finishstartclone();
finishclone( ret );

assert.eq( 100000, t.a.find().count() );
assert.eq( 1, t.a.find( { i: 200000 } ).count() );
assert.eq( 0, t.a.find( { i: -1 } ).count() );
assert.eq( 0, t.a.find( { i: 0 } ).count() );
assert.eq( 1, t.a.find( { i: 99998, x: "y" } ).count() );


// Now test oplog running out of space -- specify small size clone oplog for test.
f.a.drop();
t.a.drop();

for( i = 0; i < 200000; ++i ) {
    f.a.save( { i: i } );
}
assert.eq( 200000, f.a.count() );

startstartclone( ", logSizeMb:1" );
ret = finishstartclone();

for( i = 200000; i < 210000; ++i ) {
    f.a.save( { i: i } );
}

assert.commandFailed( dofinishclonecmd( ret ) );

// Make sure the same works with standard size op log.
f.a.drop();
t.a.drop();

for( i = 0; i < 200000; ++i ) {
    f.a.save( { i: i } );
}
assert.eq( 200000, f.a.count() );

startstartclone();
ret = finishstartclone();

for( i = 200000; i < 210000; ++i ) {
    f.a.save( { i: i } );
}
assert.eq( 210000, f.a.count() );

finishclone( ret );
assert.eq( 210000, t.a.find().count() );
    
// Test startCloneCollection and finishCloneCollection commands.
f.a.drop();
t.a.drop();

for( i = 0; i < 100000; ++i ) {
    f.a.save( { i: i } );
}
assert.eq( 100000, f.a.count() );

startstartclone();

sleep( 200 );
f.a.save( { i: -1 } );

ret = finishstartclone();
assert.eq( 100001, t.a.find().count() );

f.a.save( { i: -2 } );
assert.eq( 100002, f.a.find().count() );
finishclone( ret );
assert.eq( 100002, t.a.find().count() );
