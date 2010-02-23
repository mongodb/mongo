// test copyDatabase from an auth enabled source

ports = allocatePorts( 2 );

var baseName = "jstests_clone_copyauth";

var source = startMongod( "--auth", "--port", ports[ 0 ], "--dbpath", "/data/db/" + baseName + "_source", "--nohttpinterface", "--bind_ip", "127.0.0.1", "--smallfiles" );
var target = startMongod( "--port", ports[ 1 ], "--dbpath", "/data/db/" + baseName + "_target", "--nohttpinterface", "--bind_ip", "127.0.0.1", "--smallfiles" );

source.getDB( baseName )[ baseName ].save( {i:1} );
source.getDB( baseName ).addUser( "foo", "bar" );
source.getDB( "admin" ).addUser( "super", "super" );
assert.throws( function() { source.getDB( baseName )[ baseName ].findOne(); } );

target.getDB( baseName ).copyDatabase( baseName, baseName, source.host, "foo", "bar" );
assert.eq( 1, target.getDB( baseName )[ baseName ].count() );
assert.eq( 1, target.getDB( baseName )[ baseName ].findOne().i );

stopMongod( ports[ 1 ] );

var target = startMongod( "--auth", "--port", ports[ 1 ], "--dbpath", "/data/db/" + baseName + "_target", "--nohttpinterface", "--bind_ip", "127.0.0.1", "--smallfiles" );

target.getDB( "admin" ).addUser( "super1", "super1" );
assert.throws( function() { source.getDB( baseName )[ baseName ].findOne(); } );
target.getDB( "admin" ).auth( "super1", "super1" );

target.getDB( baseName ).copyDatabase( baseName, baseName, source.host, "foo", "bar" );
assert.eq( 1, target.getDB( baseName )[ baseName ].count() );
assert.eq( 1, target.getDB( baseName )[ baseName ].findOne().i );
