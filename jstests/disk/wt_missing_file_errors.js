/**
 * Tests that MongoDB gives errors when certain data files are missing.
 *
 * @tags: [requires_wiredtiger]
 */

(function() {

load('jstests/disk/libs/wt_file_helper.js');

const baseName = "wt_missing_file_errors";
const collName = "test";
const dbpath = MongoRunner.dataPath + baseName + "/";

/**
 * Test 1. Delete a collection's .wt file.
 */

assertErrorOnRequestWhenFilesAreCorruptOrMissing(
    dbpath,
    baseName,
    collName,
    (mongod, testColl) => {
        const testCollUri = getUriForColl(testColl);
        const testCollFile = dbpath + testCollUri + ".wt";
        MongoRunner.stopMongod(mongod);
        jsTestLog("deleting collection file: " + testCollFile);
        removeFile(testCollFile);
    },
    (testColl) => {
        assert.throws(() => {
            testColl.insert({a: 1});
        });
    },
    "Fatal Assertion 50882");

/**
 * Test 2. Delete the _mdb_catalog.
 */

assertErrorOnStartupWhenFilesAreCorruptOrMissing(dbpath, baseName, collName, (mongod, testColl) => {
    MongoRunner.stopMongod(mongod);
    let mdbCatalogFile = dbpath + "_mdb_catalog.wt";
    jsTestLog("deleting catalog file: " + mdbCatalogFile);
    removeFile(mdbCatalogFile);
}, "Fatal Assertion 50882");

/**
 * Test 3. Delete the WiredTiger.wt.
 */

assertErrorOnStartupWhenFilesAreCorruptOrMissing(dbpath, baseName, collName, (mongod, testColl) => {
    MongoRunner.stopMongod(mongod);
    let WiredTigerWTFile = dbpath + "WiredTiger.wt";
    jsTestLog("deleting WiredTiger.wt");
    removeFile(WiredTigerWTFile);
}, "Fatal Assertion 28595");

/**
 * Test 4. Delete an index file.
 */

assertErrorOnRequestWhenFilesAreCorruptOrMissing(
    dbpath,
    baseName,
    collName,
    (mongod, testColl) => {
        const indexName = "a_1";
        assert.commandWorked(testColl.createIndex({a: 1}, {name: indexName}));
        const indexUri = getUriForIndex(testColl, indexName);
        MongoRunner.stopMongod(mongod);
        const indexFile = dbpath + indexUri + ".wt";
        jsTestLog("deleting index file: " + indexFile);
        removeFile(indexFile);
    },
    (testColl) => {
        // This insert will crash the server because it triggers the code path
        // of looking for the index file.
        assert.throws(function() {
            testColl.insert({a: 1});
        });
    },
    "Fatal Assertion 50882");
})();
