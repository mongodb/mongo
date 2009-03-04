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
