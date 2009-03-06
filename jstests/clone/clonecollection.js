// Test cloneCollection command

var baseName = "jstests_clonecollection";

f = startMongod( "--port", "27018", "--dbpath", "/data/db/" + baseName + "_from" ).getDB( baseName );
t = startMongod( "--port", "27019", "--dbpath", "/data/db/" + baseName + "_to" ).getDB( baseName );

for( i = 0; i < 1000; ++i ) {
    f.a.save( { i: i } );
}
assert.eq( 1000, f.a.find().count() );

t.cloneCollection( "localhost:27018", "a" );
assert.eq( 1000, t.a.find().count() );

t.a.drop();

t.cloneCollection( "localhost:27018", "a", { i: { $gte: 10, $lt: 20 } } );
assert.eq( 10, t.a.find().count() );

t.a.drop();
assert.eq( 0, t.system.indexes.find().count() );

f.a.ensureIndex( { i: 1 } );
t.cloneCollection( "localhost:27018", "a" );
assert.eq( 1, t.system.indexes.find().count() );
// Verify index works
assert.eq( 50, t.a.find( { i: 50 } ).hint( { i: 1 } ).explain().startKey.i );
assert.eq( 1, t.a.find( { i: 50 } ).hint( { i: 1 } ).toArray().length );

f.a.drop();
t.a.drop();

for( i = 0; i < 100000; ++i ) {
    f.a.save( { i: i } );
}

finished = false
cc = fork( function() { t.cloneCollection( "localhost:27018", "a", {i:{$gte:0}} ); finished = true; } );
cc.start();

sleep( 200 );
f.a.save( { i: 200000 } );
//f.a.save( { i: -1 } );
f.a.remove( { i: 99999 } );
f.a.update( { i: 99998 }, { i: 99998, x: "y" } );
assert( !finished, "test run invalid" );

cc.join();

assert.eq( 100000, t.a.find().count() );
assert.eq( 1, t.a.find( { i: 200000 } ).count() );
assert.eq( 0, t.a.find( { i: -1 } ).count() );
assert.eq( 0, t.a.find( { i: 99999 } ).count() );
assert.eq( 1, t.a.find( { i: 99998, x: "y" } ).count() );
