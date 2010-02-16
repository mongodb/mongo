// Test SERVER-623 - starting slave from a new snapshot

ports = allocatePorts( 3 );

var baseName = "jstests_disk_slavefromsnapshot";
var basePath = "/data/db/" + baseName;

var m = startMongod( "--master", "--port", ports[ 0 ], "--dbpath", basePath + "_master", "--nohttpinterface", "--oplogSize", "1" );

big = new Array( 2000 ).toString();
for( i = 0; i < 1000; ++i )
    m.getDB( baseName )[ baseName ].save( { _id: new ObjectId(), i: i, b: big } );

m.getDB( "admin" ).runCommand( {fsync:1,lock:1} );
copyDbpath( basePath + "_master", basePath + "_slave1" );
m.getDB( "admin" ).$cmd.sys.unlock.findOne();

var s1 = startMongoProgram( "mongod", "--slave", "--source", "127.0.0.1:" + ports[ 0 ], "--port", ports[ 1 ], "--dbpath", basePath + "_slave1", "--nohttpinterface" );
assert.eq( 1000, s1.getDB( baseName )[ baseName ].count() );
m.getDB( baseName )[ baseName ].save( {i:1000} );
assert.soon( function() { return 1001 == s1.getDB( baseName )[ baseName ].count(); } );

s1.getDB( "admin" ).runCommand( {fsync:1,lock:1} );
copyDbpath( basePath + "_slave1", basePath + "_slave2" );
s1.getDB( "admin" ).$cmd.sys.unlock.findOne();

var s2 = startMongoProgram( "mongod", "--slave", "--source", "127.0.0.1:" + ports[ 0 ], "--port", ports[ 2 ], "--dbpath", basePath + "_slave2", "--nohttpinterface" );
assert.eq( 1001, s2.getDB( baseName )[ baseName ].count() );
m.getDB( baseName )[ baseName ].save( {i:1001} );
assert.soon( function() { return 1002 == s2.getDB( baseName )[ baseName ].count(); } );
assert.soon( function() { return 1002 == s1.getDB( baseName )[ baseName ].count(); } );

assert( !rawMongoProgramOutput().match( /resync: cloning database/ ) );