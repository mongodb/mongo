// repair with --directoryperdb

var baseName = "jstests_disk_repair2";

function check() {
    files = listFiles( dbpath );
    for( f in files ) {
        assert( ! new RegExp( "^" + dbpath + "backup_" ).test( files[ f ].name ),
                "backup dir " + files[ f ].name + " in dbpath" );
    }

    assert.eq.automsg( "1", "db[ baseName ].count()" );
}

port = allocatePorts( 1 )[ 0 ];
dbpath = MongoRunner.dataPath + baseName + "/";
repairpath = dbpath + "repairDir/";
longDBName = Array(61).join('a');
longRepairPath = dbpath + Array(61).join('b') + '/';

resetDbpath( dbpath );
resetDbpath( repairpath );

m = startMongoProgram( "mongod", "--directoryperdb", "--port", port, "--dbpath", dbpath, "--repairpath", repairpath, "--nohttpinterface", "--bind_ip", "127.0.0.1" );
db = m.getDB( baseName );
db[ baseName ].save( {} );
assert.commandWorked( db.runCommand( {repairDatabase:1, backupOriginalFiles:true} ) );

//Check that repair files exist in the repair directory, and nothing else
db.adminCommand( { fsync : 1 } );
files = listFiles( repairpath + "/backup_repairDatabase_0/" + baseName );
var fileCount = 0;
for( f in files ) {
    print( files[ f ].name );
    if ( files[ f ].isDirectory )
        continue;
    fileCount += 1;
    assert( /\.bak$/.test( files[ f ].name ),
            "In database repair directory, found unexpected file: " + files[ f ].name );
}
assert( fileCount > 0, "Expected more than zero nondirectory files in the database directory" );

check();
stopMongod( port );

resetDbpath( repairpath );
m = startMongoProgram( "mongod", "--directoryperdb", "--port", port, "--dbpath", dbpath, "--nohttpinterface", "--bind_ip", "127.0.0.1" );
db = m.getDB( baseName );
assert.commandWorked( db.runCommand( {repairDatabase:1} ) );
check();
stopMongod( port );

//Test long database names
resetDbpath( repairpath );
m = startMongoProgram( "mongod", "--directoryperdb", "--port", port, "--dbpath", dbpath, "--nohttpinterface", "--bind_ip", "127.0.0.1" );
db = m.getDB( longDBName );
assert.writeOK(db[ baseName ].save( {} ));
assert.commandWorked( db.runCommand( {repairDatabase:1} ) );
stopMongod( port );

//Test long repairPath
resetDbpath( longRepairPath )
m = startMongoProgram( "mongod", "--directoryperdb", "--port", port, "--dbpath", dbpath, "--repairpath", longRepairPath, "--nohttpinterface", "--bind_ip", "127.0.0.1" );
db = m.getDB( longDBName );
assert.commandWorked( db.runCommand( {repairDatabase:1, backupOriginalFiles: true} ) );
check();
stopMongod( port );

//Test database name and repairPath with --repair
resetDbpath( longRepairPath )
m = startMongoProgram( "mongod", "--repair", "--directoryperdb", "--port", port, "--dbpath", dbpath, "--repairpath", longRepairPath, "--nohttpinterface", "--bind_ip", "127.0.0.1" );
m = startMongoProgram( "mongod", "--directoryperdb", "--port", port, "--dbpath", dbpath, "--nohttpinterface", "--bind_ip", "127.0.0.1" );
db = m.getDB( longDBName );
check();
stopMongod( port );

resetDbpath( repairpath );
runMongoProgram( "mongod", "--repair", "--directoryperdb", "--port", port, "--dbpath", dbpath, "--repairpath", repairpath, "--nohttpinterface", "--bind_ip", "127.0.0.1" );
m = startMongoProgram( "mongod", "--directoryperdb", "--port", port, "--dbpath", dbpath, "--repairpath", repairpath, "--nohttpinterface", "--bind_ip", "127.0.0.1" );
db = m.getDB( baseName );
check();
stopMongod( port );

resetDbpath( repairpath );
runMongoProgram( "mongod", "--repair", "--directoryperdb", "--port", port, "--dbpath", dbpath, "--nohttpinterface", "--bind_ip", "127.0.0.1" );
m = startMongoProgram( "mongod", "--directoryperdb", "--port", port, "--dbpath", dbpath, "--nohttpinterface", "--bind_ip", "127.0.0.1" );
db = m.getDB( baseName );
check();
