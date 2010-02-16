// Test SERVER-623 - starting slave from a new snapshot

ports = allocatePorts( 3 );

var baseName = "jstests_disk_slavefromsnapshot";
var basePath = "/data/db/" + baseName;

var m = startMongod( "--master", "--port", ports[ 0 ], "--dbpath", basePath + "_master", "--nohttpinterface", "--oplogSize", "1" );

m.getDB( baseName )[ baseName ].save( {i:1} );

m.getDB( "admin" ).runCommand( {fsync:1,lock:1} );
copyDbpath( basePath + "_master", basePath + "_slave1" );
printjson( listFiles( basePath + "_slave1" ) );
m.getDB( "admin" ).$cmd.sys.unlock.findOne();

var s1 = startMongoProgram( "mongod", "--slave", "--source", "127.0.0.1:" + ports[ 0 ], "--port", ports[ 1 ], "--dbpath", basePath + "_slave1", "--nohttpinterface" );
assert.eq( 1, s1.getDB( baseName )[ baseName ].findOne().i );
m.getDB( baseName )[ baseName ].save( {i:2} );
assert.soon( function() { return 2 == s1.getDB( baseName )[ baseName ].count(); } );

s1.getDB( "admin" ).runCommand( {fsync:1,lock:1} );
copyDbpath( basePath + "_slave1", basePath + "_slave2" );
s1.getDB( "admin" ).$cmd.sys.unlock.findOne();

var s2 = startMongoProgram( "mongod", "--slave", "--source", "127.0.0.1:" + ports[ 0 ], "--port", ports[ 2 ], "--dbpath", basePath + "_slave2", "--nohttpinterface" );
assert.eq( 2, s2.getDB( baseName )[ baseName ].count() );
m.getDB( baseName )[ baseName ].save( {i:3} );
assert.soon( function() { return 3 == s2.getDB( baseName )[ baseName ].count(); } );
assert.soon( function() { return 3 == s1.getDB( baseName )[ baseName ].count(); } );
