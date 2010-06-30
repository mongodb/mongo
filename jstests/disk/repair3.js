// test --repairpath on aother partition

var baseName = "jstests_disk_repair3";
var repairbase = "/data/db/repairpartitiontest"
var repairpath = repairbase + "/dir"

doIt = false;
files = listFiles( "/data/db" );
for ( i in files ) {
    if ( files[ i ].name == repairbase ) {
        doIt = true;
    }
}

// SERVER-1217 disable test
doIt = false;

if ( !doIt ) {
    print( "path " + repairpath + " missing, skipping repair3 test" );
    doIt = false;
}

if ( doIt ) {

    port = allocatePorts( 1 )[ 0 ];
    dbpath = "/data/db/" + baseName + "/";

    resetDbpath( dbpath );
    resetDbpath( repairpath );

    m = startMongoProgram( "mongod", "--noprealloc", "--smallfiles", "--port", port, "--dbpath", dbpath, "--repairpath", repairpath, "--nohttpinterface", "--bind_ip", "127.0.0.1" );
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
    rc = runMongoProgram( "mongod", "--noprealloc", "--smallfiles", "--repair", "--port", port, "--dbpath", dbpath, "--repairpath", repairpath, "--nohttpinterface", "--bind_ip", "127.0.0.1" );
    assert.eq.automsg( "0", "rc" );
    m = startMongoProgram( "mongod", "--noprealloc", "--smallfiles", "--port", port, "--dbpath", dbpath, "--repairpath", repairpath, "--nohttpinterface", "--bind_ip", "127.0.0.1" );
    db = m.getDB( baseName );
    check();
    stopMongod( port );

}