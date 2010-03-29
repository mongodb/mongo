// Test SERVER-623 - starting repl peer from a new snapshot of slave

ports = allocatePorts( 3 );

var baseName = "repl_snapshot3";
var basePath = "/data/db/" + baseName;

a = new MongodRunner( ports[ 0 ], basePath + "-arbiter" );
l = new MongodRunner( ports[ 1 ], basePath + "-left", "127.0.0.1:" + ports[ 2 ], "127.0.0.1:" + ports[ 0 ] );
r = new MongodRunner( ports[ 2 ], basePath + "-right", "127.0.0.1:" + ports[ 1 ], "127.0.0.1:" + ports[ 0 ] ); 

rp = new ReplPair( l, r, a );
rp.start();
rp.waitForSteadyState();

big = new Array( 2000 ).toString();
rp.slave().setSlaveOk();
for( i = 0; i < 1000; ++i ) {
    rp.master().getDB( baseName )[ baseName ].save( { _id: new ObjectId(), i: i, b: big } );
    if ( i % 250 == 249 ) {
        assert.soon( function() { return i+1 == rp.slave().getDB( baseName )[ baseName ].count(); } );    
    }
}

rp.slave().getDB( "admin" ).runCommand( {fsync:1,lock:1} );
leftSlave = ( rp.slave().host == rp.left().host );
rp.killNode( rp.master() );
if ( leftSlave ) {
    copyDbpath( basePath + "-left", basePath + "-right" );
} else {
    copyDbpath( basePath + "-right", basePath + "-left" );    
}
rp.slave().getDB( "admin" ).$cmd.sys.unlock.findOne();
rp.killNode( rp.slave() );                     

clearRawMongoProgramOutput();

rp.right_.extraArgs_ = [ "--fastsync" ];
rp.left_.extraArgs_ = [ "--fastsync" ];

rp.start( true );
rp.waitForSteadyState();
assert.eq( 1000, rp.master().getDB( baseName )[ baseName ].count() );
rp.slave().setSlaveOk();
assert.eq( 1000, rp.slave().getDB( baseName )[ baseName ].count() );
rp.master().getDB( baseName )[ baseName ].save( {i:1000} );
assert.soon( function() { return 1001 == rp.slave().getDB( baseName )[ baseName ].count(); } );

assert( !rawMongoProgramOutput().match( /resync/ ) );
assert( !rawMongoProgramOutput().match( /SyncException/ ) );