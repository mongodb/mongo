// Test SERVER-623 - starting slave from a new snapshot

ports = allocatePorts( 3 );

var baseName = "repl_snapshot1";

rt1 = new ReplTest( "repl_snapshot1-1", [ ports[ 0 ], ports[ 1 ] ] );
rt2 = new ReplTest( "repl_snapshot1-2", [ ports[ 0 ], ports[ 2 ] ] );
m = rt1.start( true );

big = new Array( 2000 ).toString();
for( i = 0; i < 1000; ++i )
    m.getDB( baseName )[ baseName ].save( { _id: new ObjectId(), i: i, b: big } );

m.getDB( "admin" ).runCommand( {fsync:1,lock:1} );
copyDbpath( rt1.getPath( true ), rt1.getPath( false ) );
m.getDB( "admin" ).$cmd.sys.unlock.findOne();

s1 = rt1.start( false, null, true );
assert.eq( 1000, s1.getDB( baseName )[ baseName ].count() );
m.getDB( baseName )[ baseName ].save( {i:1000} );
assert.soon( function() { return 1001 == s1.getDB( baseName )[ baseName ].count(); } );

s1.getDB( "admin" ).runCommand( {fsync:1,lock:1} );
copyDbpath( rt1.getPath( false ), rt2.getPath( false ) );
s1.getDB( "admin" ).$cmd.sys.unlock.findOne();

s2 = rt2.start( false, null, true );
assert.eq( 1001, s2.getDB( baseName )[ baseName ].count() );
m.getDB( baseName )[ baseName ].save( {i:1001} );
assert.soon( function() { return 1002 == s2.getDB( baseName )[ baseName ].count(); } );
assert.soon( function() { return 1002 == s1.getDB( baseName )[ baseName ].count(); } );

assert( !rawMongoProgramOutput().match( /resync/ ) );