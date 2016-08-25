// dumprestore.js

load('./jstests/multiVersion/libs/dumprestore_helpers.js');

// The base name to use for various things in the test, including the dbpath and the database name
var testBaseName = "jstests_tool_dumprestore";

// Paths to external directories to be used to store dump files
var dumpDir = MongoRunner.dataPath + testBaseName + "_dump_external/";
var testDbpath = MongoRunner.dataPath + testBaseName + "_dbpath_external/";

// Start with basic multiversion tests just running against a single mongod
var singleNodeTests = {
    'serverSourceVersion': ["latest", "last-stable"],
    'serverDestVersion': ["latest", "last-stable"],
    'mongoDumpVersion': ["latest", "last-stable"],
    'mongoRestoreVersion': ["latest", "last-stable"],
    'dumpDir': [dumpDir],
    'testDbpath': [testDbpath],
    'dumpType': ["mongod"],
    'restoreType': ["mongod"],
    'storageEngine': [jsTest.options().storageEngine || "wiredTiger"]
};
runAllDumpRestoreTests(singleNodeTests);
