// test that disk space check happens on --repairpath partition

var baseName = "jstests_disk_repair4";
var smallbase = MongoRunner.dataDir + "/repairpartitiontest"
var smallpath = smallbase + "/dir"

doIt = false;
files = listFiles( MongoRunner.dataDir );
for ( i in files ) {
    if ( files[ i ].name == smallbase ) {
        doIt = true;
    }
}

if ( !doIt ) {
    print( "path " + smallpath + " missing, skipping repair4 test" );
    doIt = false;
}

if ( doIt ) {

    port = allocatePorts( 1 )[ 0 ];
    repairpath = MongoRunner.dataPath + baseName + "/";
    
    resetDbpath( smallpath );
    resetDbpath( repairpath );
    
    m = startMongoProgram( "mongod", "--nssize", "8", "--noprealloc", "--smallfiles", "--port", port, "--dbpath", smallpath, "--repairpath", repairpath, "--nohttpinterface", "--bind_ip", "127.0.0.1" );
    db = m.getDB( baseName );
    db[ baseName ].save( {} );
    assert.commandWorked( db.runCommand( {repairDatabase:1, backupOriginalFiles:true} ) );
    function check() {
        files = listFiles( smallpath );
        for( f in files ) {
            assert( ! new RegExp( "^" + smallpath + "backup_" ).test( files[ f ].name ), "backup dir in dbpath" );
        }
        
        assert.eq.automsg( "1", "db[ baseName ].count()" );
    }
    
    check();
    MongoRunner.stopMongod( port );

}