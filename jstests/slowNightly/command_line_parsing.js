// validate command line parameter parsing

port = allocatePorts( 1 )[ 0 ];
var baseName = "jstests_slowNightly_command_line_parsing";

// test notablescan
var m = startMongod( "--port", port, "--dbpath", "/data/db/" + baseName, "--notablescan" );
m.getDB( baseName ).getCollection( baseName ).save( {a:1} );
assert.throws( function() { m.getDB( baseName ).getCollection( baseName ).find( {a:1} ).toArray() } );

// test config file 
var m2 = startMongod( "--port", port+2, "--dbpath", "/data/db/" + baseName +"2", "--config", "jstests/libs/testconfig");

var m2expected = {
    "parsed" : {
        "config" : "jstests/libs/testconfig",
        "dbpath" : "/data/db/jstests_slowNightly_command_line_parsing2",
        "fastsync" : true,
        "port" : 31002,
    }
};
var m2result = m2.getDB("admin").runCommand( "getCmdLineOpts" );

//remove setParameter as it is variable depending on the way the test is started.
delete m2result.parsed.setParameter
assert.docEq( m2expected.parsed, m2result.parsed );

// test JSON config file
var m3 = startMongod("--port", port+4, "--dbpath", "/data/db/" + baseName +"4",
                     "--config", "jstests/libs/testconfig");

var m3expected = {
    "parsed" : {
        "config" : "jstests/libs/testconfig",
        "dbpath" : "/data/db/jstests_slowNightly_command_line_parsing4",
        "fastsync" : true,
        "port" : 31004,
    }
};
var m3result = m3.getDB("admin").runCommand( "getCmdLineOpts" );

//remove setParameter as it is variable depending on the way the test is started.
delete m3result.parsed.setParameter
assert.docEq( m3expected.parsed, m3result.parsed );
