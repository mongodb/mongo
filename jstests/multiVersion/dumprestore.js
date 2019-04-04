// dumprestore.js

load('./jstests/multiVersion/libs/dumprestore_helpers.js');

// The base name to use for various things in the test, including the dbpath and the database name
var testBaseName = "jstests_tool_dumprestore";

// Paths to external directories to be used to store dump files
var dumpDir = MerizoRunner.dataPath + testBaseName + "_dump_external/";
var testDbpath = MerizoRunner.dataPath + testBaseName + "_dbpath_external/";

// Start with basic multiversion tests just running against a single merizod
var singleNodeTests = {
    'serverSourceVersion': ["latest", "last-stable"],
    'serverDestVersion': ["latest", "last-stable"],
    'merizoDumpVersion': ["latest", "last-stable"],
    'merizoRestoreVersion': ["latest", "last-stable"],
    'dumpDir': [dumpDir],
    'testDbpath': [testDbpath],
    'dumpType': ["merizod"],
    'restoreType': ["merizod"],
    'storageEngine': [jsTest.options().storageEngine || "wiredTiger"]
};
runAllDumpRestoreTests(singleNodeTests);
