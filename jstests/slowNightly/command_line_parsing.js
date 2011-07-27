// validate command line parameter parsing

port = allocatePorts( 1 )[ 0 ];
var baseName = "jstests_slowNightly_command_line_parsing";

// test notablescan
var m = startMongod( "--port", port, "--dbpath", "/data/db/" + baseName, "--notablescan" );
m.getDB( baseName ).getCollection( baseName ).save( {a:1} );
assert.throws( function() { m.getDB( baseName ).getCollection( baseName ).find( {a:1} ).toArray() } );

// test config file 
var m2 = startMongod( "--port", port+2, "--dbpath", "/data/db/" + baseName +"2", "--config", "jstests/libs/testconfig");
var m2result = {
    "parsed" : {
        "config" : "jstests/libs/testconfig",
        "dbpath" : "/data/db/jstests_slowNightly_command_line_parsing2",
        "fastsync" : "true",
        "port" : 31002
    }
};
assert( friendlyEqual(m2result.parsed, m2.getDB("admin").runCommand( "getCmdLineOpts" ).parsed) );
