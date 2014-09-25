// dumprestore_direct.js

load( './jstests/multiVersion/libs/dumprestore_helpers.js' )


// The base name to use for various things in the test, including the dbpath and the database name
var testBaseName = "jstests_tool_dumprestore_direct";

// Paths to external directories to be used to store dump files
var dumpDir = MongoRunner.dataPath + testBaseName + "_dump_external/";
var testDbpath = MongoRunner.dataPath + testBaseName + "_dbpath_external/";



// Test dumping directly from data files across versions
var directDumpTests = {
    'serverSourceVersion' : [ "latest", "last-stable" ],
    'serverDestVersion' :[ "latest", "last-stable" ],
    'mongoDumpVersion' :[ "latest", "last-stable" ],
    'mongoRestoreVersion' :[ "latest", "last-stable" ],
    'dumpDir' : [ dumpDir ],
    'testDbpath' : [ testDbpath ],
    'dumpType' : [ "direct" ],
    'restoreType' : [ "mongod" ]
};
runAllDumpRestoreTests(directDumpTests);



// Test restoring directly to data files across versions
var directRestoreTests = {
    'serverSourceVersion' : [ "latest", "last-stable" ],
    'serverDestVersion' :[ "latest", "last-stable" ],
    'mongoDumpVersion' :[ "latest", "last-stable" ],
    'mongoRestoreVersion' :[ "latest", "last-stable" ],
    'dumpDir' : [ dumpDir ],
    'testDbpath' : [ testDbpath ],
    'dumpType' : [ "mongod" ],
    'restoreType' : [ "direct" ]
};
runAllDumpRestoreTests(directRestoreTests);

