// Test basic replication functionality

var baseName = "jstests_repl1test";

m = startMongod( "--port", "27018", "--dbpath", "/data/db/" + baseName + "-master", "--master" );
s = startMongod( "--port", "27019", "--dbpath", "/data/db/" + baseName + "-slave", "--slave", "--source", "127.0.0.1:27018" );

am = m.getDB( baseName ).a
as = s.getDB( baseName ).a

for( i = 0; i < 1000; ++i )
    am.save( { _id: new ObjectId(), i: i } );

assert.soon( function() { return as.find().count() == 1000; } );

assert.eq( 1, as.find( { i: 0 } ).count() );
assert.eq( 1, as.find( { i: 999 } ).count() );
