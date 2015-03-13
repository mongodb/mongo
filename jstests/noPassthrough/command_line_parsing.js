// validate command line parameter parsing

port = allocatePorts( 1 )[ 0 ];
var baseName = "jstests_slowNightly_command_line_parsing";

// test notablescan
var m = startMongod( "--port", port, "--dbpath", MongoRunner.dataPath + baseName, "--notablescan" );
m.getDB( baseName ).getCollection( baseName ).save( {a:1} );
assert.throws( function() { m.getDB( baseName ).getCollection( baseName ).find( {a:1} ).toArray() } );

// test config file 
var m2 = startMongod( "--port", port+2, "--dbpath", MongoRunner.dataPath + baseName +"2", "--config", "jstests/libs/testconfig");

var m2expected = {
    "parsed" : {
        "config" : "jstests/libs/testconfig",
        "storage" : {
            "dbPath" : MongoRunner.dataDir + "/jstests_slowNightly_command_line_parsing2",
        },
        "net" : {
            "port" : 31002
        },
        "help" : false,
        "version" : false,
        "sysinfo" : false
    }
};
var m2result = m2.getDB("admin").runCommand( "getCmdLineOpts" );

// remove variables that depend on the way the test is started.
delete m2result.parsed.setParameter
delete m2result.parsed.storage.engine
delete m2result.parsed.storage.wiredTiger
assert.docEq( m2expected.parsed, m2result.parsed );

// test JSON config file
var m3 = startMongod("--port", port+4, "--dbpath", MongoRunner.dataPath + baseName +"4",
                     "--config", "jstests/libs/testconfig");

var m3expected = {
    "parsed" : {
        "config" : "jstests/libs/testconfig",
        "storage" : {
            "dbPath" : MongoRunner.dataDir + "/jstests_slowNightly_command_line_parsing4",
        },
        "net" : {
            "port" : 31004
        },
        "help" : false,
        "version" : false,
        "sysinfo" : false
    }
};
var m3result = m3.getDB("admin").runCommand( "getCmdLineOpts" );

// remove variables that depend on the way the test is started.
delete m3result.parsed.setParameter
delete m3result.parsed.storage.engine
delete m3result.parsed.storage.wiredTiger
assert.docEq( m3expected.parsed, m3result.parsed );
