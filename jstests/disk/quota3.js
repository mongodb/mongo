// Test for quotaFiles being ignored allocating a large collection - SERVER-3511.

if ( 0 ) { // SERVER-3511

port = allocatePorts( 1 )[ 0 ];

baseName = "jstests_disk_quota3";
dbpath = "/data/db/" + baseName;

m = startMongod( "--port", port, "--dbpath", "/data/db/" + baseName, "--quotaFiles", "3", "--smallfiles" );
db = m.getDB( baseName );

db.createCollection( baseName, {size:128*1024*1024} );
assert( db.getLastError() );

dotFourDataFile = dbpath + "/" + baseName + ".4";
files = listFiles( dbpath );
for( i in files ) {
    // .3 file may be preallocated but not .4
	assert.neq( dotFourDataFile, files[ i ].name );
}

}