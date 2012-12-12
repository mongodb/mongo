// SERVER-2351 Test killop with repair command.

var baseName = "jstests_disk_repair5";

port = allocatePorts( 1 )[ 0 ];
dbpath = "/data/db/" + baseName + "/";
repairpath = dbpath + "repairDir/"

resetDbpath( dbpath );
resetDbpath( repairpath );

m = startMongodTest(port,
                    baseName + "/",
                    true,
                    {repairpath : repairpath, nohttpinterface : "", bind_ip : "127.0.0.1"});

db = m.getDB( baseName );

big = new Array( 5000 ).toString();
for( i = 0; i < 20000; ++i ) {
	db[ baseName ].save( {i:i,b:big} );
}

function killRepair() {
    while( 1 ) {
     	p = db.currentOp().inprog;
        for( var i in p ) {
            var o = p[ i ];
            printjson( o );
            // Find the active 'repairDatabase' op and kill it.
            if ( o.active && o.query && o.query.repairDatabase ) {
             	db.killOp( o.opid );
                return;
            }
        }
    }
}

s = startParallelShell( killRepair.toString() + "; killRepair();" );

sleep(100); // make sure shell is actually running, lame

// Repair should fail due to killOp.
assert.commandFailed( db.runCommand( {repairDatabase:1, backupOriginalFiles:true} ) );

s();

assert.eq( 20000, db[ baseName ].find().itcount() );
assert( db[ baseName ].validate().valid );

stopMongod( port )
