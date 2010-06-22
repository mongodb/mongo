// repair with --directoryperdb

var baseName = "jstests_disk_repair2";

port = allocatePorts( 1 )[ 0 ];
dbpath = "/data/db/" + baseName + "/";
repairpath = dbpath + "repairDir/"

resetDbpath( dbpath );
resetDbpath( repairpath );

m = startMongoProgram( "mongod", "--directoryperdb", "--port", port, "--dbpath", dbpath, "--repairpath", repairpath, "--nohttpinterface", "--bind_ip", "127.0.0.1" );
db = m.getDB( baseName );
db[ baseName ].save( {} );
assert.commandWorked( db.runCommand( {repairDatabase:1, backupOriginalFiles:true} ) );
function check() {
    files = listFiles( dbpath );
    for( f in files ) {
        assert( ! new RegExp( "^" + dbpath + "backup_" ).test( files[ f ].name ), "backup dir in dbpath" );
    }
    
    assert.eq.automsg( "1", "db[ baseName ].count()" );
}
check();
stopMongod( port );

resetDbpath( repairpath );
m = startMongoProgram( "mongod", "--directoryperdb", "--port", port, "--dbpath", dbpath, "--nohttpinterface", "--bind_ip", "127.0.0.1" );
db = m.getDB( baseName );
assert.commandWorked( db.runCommand( {repairDatabase:1} ) );
check();
stopMongod( port );

resetDbpath( repairpath );
rc = runMongoProgram( "mongod", "--repair", "--directoryperdb", "--port", port, "--dbpath", dbpath, "--repairpath", repairpath, "--nohttpinterface", "--bind_ip", "127.0.0.1" );
assert.eq.automsg( "0", "rc" );
m = startMongoProgram( "mongod", "--directoryperdb", "--port", port, "--dbpath", dbpath, "--repairpath", repairpath, "--nohttpinterface", "--bind_ip", "127.0.0.1" );
db = m.getDB( baseName );
check();
stopMongod( port );

resetDbpath( repairpath );
rc = runMongoProgram( "mongod", "--repair", "--directoryperdb", "--port", port, "--dbpath", dbpath, "--nohttpinterface", "--bind_ip", "127.0.0.1" );
assert.eq.automsg( "0", "rc" );
m = startMongoProgram( "mongod", "--directoryperdb", "--port", port, "--dbpath", dbpath, "--nohttpinterface", "--bind_ip", "127.0.0.1" );
db = m.getDB( baseName );
check();
