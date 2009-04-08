// Test cloneCollection command

var baseName = "jstests_clonecollection";

f = startMongod( "--port", "27018", "--dbpath", "/data/db/" + baseName + "_from" ).getDB( baseName );
t = startMongod( "--port", "27019", "--dbpath", "/data/db/" + baseName + "_to" ).getDB( baseName );

for( i = 0; i < 1000; ++i ) {
    f.a.save( { i: i } );
}
assert.eq( 1000, f.a.find().count() );

assert.commandWorked( t.cloneCollection( "localhost:27018", "a" ) );
assert.eq( 1000, t.a.find().count() );

t.a.drop();

assert.commandWorked( t.cloneCollection( "localhost:27018", "a", { i: { $gte: 10, $lt: 20 } } ) );
assert.eq( 10, t.a.find().count() );

t.a.drop();
assert.eq( 0, t.system.indexes.find().count() );

f.a.ensureIndex( { i: 1 } );
assert.commandWorked( t.cloneCollection( "localhost:27018", "a" ) );
assert.eq( 1, t.system.indexes.find().count(), "expected index missing" );
// Verify index works
assert.eq( 50, t.a.find( { i: 50 } ).hint( { i: 1 } ).explain().startKey.i );
assert.eq( 1, t.a.find( { i: 50 } ).hint( { i: 1 } ).toArray().length, "match length did not match expected" );


// Now test insert + delete + update during clone
f.a.drop();
t.a.drop();

for( i = 0; i < 100000; ++i ) {
    f.a.save( { i: i } );
}

finished = false;
cc = fork( function() { assert.commandWorked( t.cloneCollection( "localhost:27018", "a", {i:{$gte:0}} ) ); finished = true; } );
cc.start();

sleep( 200 );
f.a.save( { i: 200000 } );
f.a.save( { i: -1 } );
f.a.remove( { i: 0 } );
f.a.update( { i: 99998 }, { i: 99998, x: "y" } );
assert( !finished, "test run invalid" );

cc.join();

assert.eq( 100000, t.a.find().count() );
assert.eq( 1, t.a.find( { i: 200000 } ).count() );
assert.eq( 0, t.a.find( { i: -1 } ).count() );
assert.eq( 0, t.a.find( { i: 0 } ).count() );
assert.eq( 1, t.a.find( { i: 99998, x: "y" } ).count() );


// Now test oplog running out of space -- specify small size clone oplog for test.
f.a.drop();
t.a.drop();

for( i = 0; i < 100000; ++i ) {
    f.a.save( { i: i } );
}

cc = fork( function() { assert.commandFailed( t.runCommand( { cloneCollection:"jstests_clonecollection.a", from:"localhost:27018", logSizeMb:1 } ) ); } );
cc.start();

sleep( 200 );
for( i = 100000; i < 110000; ++i ) {
    f.a.save( { i: i } );
}

cc.join();


// Make sure the same works with standard size op log.
f.a.drop();
t.a.drop();

for( i = 0; i < 100000; ++i ) {
    f.a.save( { i: i } );
}

cc = fork( function() { assert.commandWorked( t.cloneCollection( "localhost:27018", "a" ) ); } );
cc.start();

sleep( 200 );
for( i = 100000; i < 110000; ++i ) {
    f.a.save( { i: i } );
}

cc.join();
assert.eq( 110000, t.a.find().count() );

// Test startCloneCollection and finishCloneCollection commands.
f.a.drop();
t.a.drop();

for( i = 0; i < 100000; ++i ) {
    f.a.save( { i: i } );
}

cc = fork( function() { return t.runCommand( {startCloneCollection:"jstests_clonecollection.a", from:"localhost:27018" } ); } );
cc.start();

sleep( 200 );
f.a.save( { i: -1 } );

ret = cc.returnData()
assert.commandWorked( ret );
assert.eq( 100001, t.a.find().count() );

f.a.save( { i: -2 } );
finishToken = ret.finishToken;
// Round-tripping through JS can corrupt the cursor ids we store as BSON
// Date elements.  Date( 0 ) will correspond to a cursorId value of 0, which
// makes the db start scanning from the beginning of the collection.
finishToken.cursorId = new Date( 0 );
assert.commandWorked( t.runCommand( {finishCloneCollection:finishToken} ) );
assert.eq( 100002, t.a.find().count() );
