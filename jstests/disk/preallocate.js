// check that there is preallocation, and there are 2 files

port = allocatePorts( 1 )[ 0 ];

var baseName = "jstests_preallocate";

var m = startMongod( "--port", port, "--dbpath", "/data/db/" + baseName );

var getTotalNonLocalSize = function() {
    var totalNonLocalDBSize = 0;
    m.getDBs().databases.forEach( function(dbStats) {
            if (dbStats.name != "local")
                totalNonLocalDBSize += dbStats.sizeOnDisk;
    });
    return totalNonLocalDBSize;
}

assert.eq( 0, getTotalNonLocalSize() );

m.getDB( baseName ).createCollection( baseName + "1" );

// Windows does not currently use preallocation
expectedMB = 64 + 16;
if ( m.getDB( baseName ).serverBits() < 64 )
    expectedMB /= 4;

assert.soon(function() { return getTotalNonLocalSize() >= expectedMB * 1024 * 1024; },
            "\n\n\nFAIL preallocate.js expected second file to bring total size over " +
            expectedMB + "MB" );

stopMongod( port );

var m = startMongoProgram( "mongod", "--port", port, "--dbpath", "/data/db/" + baseName );

size = getTotalNonLocalSize();

m.getDB( baseName ).createCollection( baseName + "2" );

sleep( 2000 ); // give prealloc a chance

assert.eq( size, getTotalNonLocalSize() );
