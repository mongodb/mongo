// dumprestore_24_sharded.js

load( './jstests/multiVersion/libs/dumprestore_helpers.js' )


// The base name to use for various things in the test, including the dbpath and the database name
var testBaseName = "jstests_tool_dumprestore_24_sharded";

// Paths to external directories to be used to store dump files
var dumpDir = MongoRunner.dataPath + testBaseName + "_dump_external/";
var testDbpath = MongoRunner.dataPath + testBaseName + "_dbpath_external/";



// Test dumping from a sharded cluster across versions
var shardedDumpTests = {
    'serverSourceVersion' : [ "latest", "2.4" ],
    'serverDestVersion' :[ "latest", "2.4" ],
    'mongoDumpVersion' :[ "latest", "2.4" ],
    'mongoRestoreVersion' :[ "latest", "2.4" ],
    'dumpDir' : [ dumpDir ],
    'testDbpath' : [ testDbpath ],
    'dumpType' : [ "mongos" ],
    'restoreType' : [ "mongod" ]
};
runAllDumpRestoreTests(shardedDumpTests);



// Test restoring to a sharded cluster across versions
var shardedRestoreTests = {
    'serverSourceVersion' : [ "latest", "2.4" ],
    'serverDestVersion' :[ "latest", "2.4" ],
    'mongoDumpVersion' :[ "latest", "2.4" ],
    'mongoRestoreVersion' :[ "latest", "2.4" ],
    'dumpDir' : [ dumpDir ],
    'testDbpath' : [ testDbpath ],
    'dumpType' : [ "mongod" ],
    'restoreType' : [ "mongos" ]
};
runAllDumpRestoreTests(shardedRestoreTests);



print("dumprestore_24_sharded success!");
