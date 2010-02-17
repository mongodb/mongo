// Test SERVER-623 - starting repl peer from a new snapshot of master

ports = allocatePorts( 3 );

var baseName = "repl_snapshot2";
var basePath = "/data/db/" + baseName;

a = new MongodRunner( ports[ 0 ], basePath + "-arbiter" );
l = new MongodRunner( ports[ 1 ], basePath + "-left", "127.0.0.1:" + ports[ 2 ], "127.0.0.1:" + ports[ 0 ] );
r = new MongodRunner( ports[ 2 ], basePath + "-right", "127.0.0.1:" + ports[ 1 ], "127.0.0.1:" + ports[ 0 ] ); 

rp = new ReplPair( l, r, a );
rp.start();
rp.waitForSteadyState();

big = new Array( 2000 ).toString();
for( i = 0; i < 1000; ++i )
    rp.master().getDB( baseName )[ baseName ].save( { _id: new ObjectId(), i: i, b: big } );

rp.master().getDB( "admin" ).runCommand( {fsync:1,lock:1} );
leftMaster = ( rp.master().host == rp.left().host );
rp.killNode( rp.slave() );
if ( leftMaster ) {
    copyDbpath( basePath + "-left", basePath + "-right" );
    rp.right_.extraArgs_ = [ "--fastsync" ];
} else {
    copyDbpath( basePath + "-right", basePath + "-left" );    
    rp.left_.extraArgs_ = [ "--fastsync" ];
}
rp.master().getDB( "admin" ).$cmd.sys.unlock.findOne();
assert.commandWorked( rp.master().getDB( "admin" ).runCommand( {replacepeer:1} ) );
rp.killNode( rp.master() );                     

rp.start( true );
rp.waitForSteadyState();
assert.eq( 1000, rp.master().getDB( baseName )[ baseName ].count() );
rp.slave().setSlaveOk();
assert.eq( 1000, rp.slave().getDB( baseName )[ baseName ].count() );
rp.master().getDB( baseName )[ baseName ].save( {i:1000} );
assert.soon( function() { return 1001 == rp.slave().getDB( baseName )[ baseName ].count(); } );

assert( !rawMongoProgramOutput().match( /resync/ ) );
assert( !rawMongoProgramOutput().match( /SyncException/ ) );