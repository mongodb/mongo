// dumprestore_24_direct.js

load( './jstests/multiVersion/libs/dumprestore_helpers.js' )


// The base name to use for various things in the test, including the dbpath and the database name
var testBaseName = "jstests_tool_dumprestore_24_direct";

// Paths to external directories to be used to store dump files
var dumpDir = MongoRunner.dataPath + testBaseName + "_dump_external/";
var testDbpath = MongoRunner.dataPath + testBaseName + "_dbpath_external/";



// Test dumping directly from data files across versions
var directDumpTests = {
    'serverSourceVersion' : [ "latest", "2.4" ],
    'serverDestVersion' :[ "latest", "2.4" ],
    'mongoDumpVersion' :[ "latest", "2.4" ],
    'mongoRestoreVersion' :[ "latest", "2.4" ],
    'dumpDir' : [ dumpDir ],
    'testDbpath' : [ testDbpath ],
    'dumpType' : [ "direct" ],
    'restoreType' : [ "mongod" ]
};
runAllDumpRestoreTests(directDumpTests);



// Test restoring directly to data files across versions
var directRestoreTests = {
    'serverSourceVersion' : [ "latest", "2.4" ],
    'serverDestVersion' :[ "latest", "2.4" ],
    'mongoDumpVersion' :[ "latest", "2.4" ],
    'mongoRestoreVersion' :[ "latest", "2.4" ],
    'dumpDir' : [ dumpDir ],
    'testDbpath' : [ testDbpath ],
    'dumpType' : [ "mongod" ],
    'restoreType' : [ "direct" ]
};
runAllDumpRestoreTests(directRestoreTests);



print("dumprestore_24_direct success!");
