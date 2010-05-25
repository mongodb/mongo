// check that there is preallocation on insert

port = allocatePorts( 1 )[ 0 ];

var baseName = "jstests_preallocate2";

var m = startMongod( "--port", port, "--dbpath", "/data/db/" + baseName );

m.getDB( baseName )[ baseName ].save( {i:1} );

assert.soon( function() { return m.getDBs().totalSize > 100000000; }, "expected second file to bring total size over 100MB" );