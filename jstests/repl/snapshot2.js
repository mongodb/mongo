// Test SERVER-623 - starting repl peer from a new snapshot of master

print("snapshot2.js 1 -----------------------------------------------------------");

ports = allocatePorts( 3 );

var baseName = "repl_snapshot2";
var basePath = "/data/db/" + baseName;

a = new MongodRunner( ports[ 0 ], basePath + "-arbiter" );
l = new MongodRunner( ports[ 1 ], basePath + "-left", "127.0.0.1:" + ports[ 2 ], "127.0.0.1:" + ports[ 0 ] );
r = new MongodRunner( ports[ 2 ], basePath + "-right", "127.0.0.1:" + ports[ 1 ], "127.0.0.1:" + ports[ 0 ] );

print("snapshot2.js 2 -----------------------------------------------------------");

rp = new ReplPair(l, r, a);
rp.start();
print("snapshot2.js 3 -----------------------------------------------------------");
rp.waitForSteadyState();

print("snapshot2.js 4 -----------------------------------------------------------");

big = new Array( 2000 ).toString(); // overflow oplog, so test can't pass supriously
rp.slave().setSlaveOk();
print("snapshot2.js 5 -----------------------------------------------------------");
for (i = 0; i < 500; ++i) {
    rp.master().getDB( baseName )[ baseName ].save( { _id: new ObjectId(), i: i, b: big } );
    if (i % 250 == 249) {
        function p() { return i + 1 == rp.slave().getDB(baseName)[baseName].count(); }
        try {
            assert.soon(p);
        } catch (e) {
            print("\n\n\nsnapshot2.js\ni+1:" + (i + 1));
            print("slave count:" + rp.slave().getDB(baseName)[baseName].count());
            sleep(2000);
            print(p());
            throw (e);
        } 
        sleep( 10 ); // give master a chance to grab a sync point - have such small oplogs the master log might overflow otherwise
    }
}
print("snapshot2.js 6 -----------------------------------------------------------");

rp.master().getDB( "admin" ).runCommand( {fsync:1,lock:1} );
leftMaster = ( rp.master().host == rp.left().host );
rp.killNode( rp.slave() );
if ( leftMaster ) {
    copyDbpath( basePath + "-left", basePath + "-right" );
} else {
    copyDbpath( basePath + "-right", basePath + "-left" );    
}
rp.master().getDB( "admin" ).$cmd.sys.unlock.findOne();
rp.killNode( rp.master() );                     

clearRawMongoProgramOutput();

rp.right_.extraArgs_ = [ "--fastsync" ];
rp.left_.extraArgs_ = [ "--fastsync" ];

rp.start( true );
rp.waitForSteadyState();
assert.eq( 500, rp.master().getDB( baseName )[ baseName ].count() );
rp.slave().setSlaveOk();
assert.eq( 500, rp.slave().getDB( baseName )[ baseName ].count() );
rp.master().getDB( baseName )[ baseName ].save( {i:500} );
assert.soon( function() { return 501 == rp.slave().getDB( baseName )[ baseName ].count(); } );

assert( !rawMongoProgramOutput().match( /resync/ ) );
assert(!rawMongoProgramOutput().match(/SyncException/));

print("snapshot2.js SUCCESS ----------------");

