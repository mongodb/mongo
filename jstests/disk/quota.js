// Check functioning of --quotaFiles parameter, including with respect to SERVER-3293 ('local' database).

port = allocatePorts( 1 )[ 0 ];

baseName = "jstests_disk_quota";
dbpath = "/data/db/" + baseName;

m = startMongod( "--port", port, "--dbpath", "/data/db/" + baseName, "--quotaFiles", "2", "--smallfiles" );
db = m.getDB( baseName );

big = new Array( 10000 ).toString();

// Insert documents until quota is exhausted.
while( !db.getLastError() ) {
    db[ baseName ].save( {b:big} );
}
printjson( db.getLastError() );

dotTwoDataFile = dbpath + "/" + baseName + ".2";
files = listFiles( dbpath );
for( i in files ) {
    // Since only one data file is allowed, a .0 file is expected and a .1 file may be preallocated (SERVER-3410) but no .2 file is expected.
	assert.neq( dotTwoDataFile, files[ i ].name );
}

dotTwoDataFile = dbpath + "/" + "local" + ".2";
// Check that quota does not apply to local db, and a .2 file can be created.
l = m.getDB( "local" )[ baseName ];
for( i = 0; i < 10000; ++i ) {
    l.save( {b:big} );
    assert( !db.getLastError() );
	dotTwoFound = false;
    if ( i % 100 != 0 ) {
        continue;
    }
    files = listFiles( dbpath );
    for( f in files ) {
     	if ( files[ f ].name == dotTwoDataFile ) {
         	dotTwoFound = true;
        }
    }
    if ( dotTwoFound ) {
     	break;   
    }
}

assert( dotTwoFound );
