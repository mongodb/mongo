// check that there is preallocation on explicit createCollection() and no unncessary preallocation after restart

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
expectedMB = ( _isWindows() ? 70 : 100 );
if ( m.getDB( baseName ).serverBits() < 64 )
    expectedMB /= 4;

assert.soon(function() { return getTotalNonLocalSize() > expectedMB * 1000000; },
            "\n\n\nFAIL preallocate.js expected second file to bring total size over " +
            expectedMB + "MB" );

stopMongod( port );

var m = startMongoProgram( "mongod", "--port", port, "--dbpath", "/data/db/" + baseName );

size = getTotalNonLocalSize();

m.getDB( baseName ).createCollection( baseName + "2" );

sleep( 2000 ); // give prealloc a chance

assert.eq( size, getTotalNonLocalSize() );
