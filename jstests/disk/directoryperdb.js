var baseDir = "jstests_disk_directoryper";
var baseName = "directoryperdb"
port = allocatePorts( 1 )[ 0 ];
dbpath = MongoRunner.dataPath + baseDir + "/";

var m = startMongodTest(port, baseDir, false, {directoryperdb : "", nohttpinterface : "", bind_ip : "127.0.0.1"});
db = m.getDB( baseName );
db[ baseName ].save( {} );
assert.eq( 1, db[ baseName ].count() , "A : " + tojson( db[baseName].find().toArray() ) );

checkDir = function( dir ) {
    db.adminCommand( {fsync:1} );
    files = listFiles( dir );
    found = false;
    for( f in files ) {
        if ( new RegExp( baseName ).test( files[ f ].name ) ) {
            found = true;
            assert( files[ f ].isDirectory, "file not directory" );
        }
    }
    assert( found, "no directory" );

    files = listFiles( dir + baseName );
    for( f in files ) {
        if ( files[f].isDirectory )
            continue;
        assert( new RegExp( baseName + "/" + baseName + "." ).test( files[ f ].name ) , "B dir:" + dir + " f: " + f );
    }
}
checkDir( dbpath );

// file iterator
assert( m.getDBs().totalSize > 0, "bad size calc" );

// repair
db.runCommand( {repairDatabase:1, backupOriginalFiles:true} );
checkDir( dbpath );
// data directory is always cleared by startMongodTest()
var backupDir = dbpath + "/backup_repairDatabase_0/";
checkDir( backupDir );
assert.eq( 1, db[ baseName ].count() , "C" );

// drop db test
db.dropDatabase();
files = listFiles( dbpath );
files.forEach( function( f ) { assert( !new RegExp( baseName ).test( f.name ), "drop database - dir not cleared" ); } );

print("SUCCESS directoryperdb.js");
