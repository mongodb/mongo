// validate command line parameter parsing

port = allocatePorts( 1 )[ 0 ];
var baseName = "jstests_slowNightly_command_line_parsing";

// test notablescan
var m = startMongod( "--port", port, "--dbpath", "/data/db/" + baseName, "--notablescan" );
m.getDB( baseName ).getCollection( baseName ).save( {a:1} );
assert.throws( function() { m.getDB( baseName ).getCollection( baseName ).find( {a:1} ).toArray() } );
