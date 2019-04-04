/**
 * Tests that MerizoDB gives errors when certain data files are corrupted.
 *
 * @tags: [requires_wiredtiger]
 */

(function() {

    load('jstests/disk/libs/wt_file_helper.js');

    const baseName = "wt_corrupt_file_errors";
    const collName = "test";
    const dbpath = MongoRunner.dataPath + baseName + "/";

    /**
     * Test 1. Corrupt a collection's .wt file.
     */

    assertErrorOnStartupWhenFilesAreCorruptOrMissing(
        dbpath, baseName, collName, (merizod, testColl) => {
            const testCollUri = getUriForColl(testColl);
            const testCollFile = dbpath + testCollUri + ".wt";
            MongoRunner.stopMongod(merizod);
            jsTestLog("corrupting collection file: " + testCollFile);
            corruptFile(testCollFile);
        }, "Fatal Assertion 50882");

    /**
     * Test 2. Corrupt the _mdb_catalog.
     */

    assertErrorOnStartupWhenFilesAreCorruptOrMissing(
        dbpath, baseName, collName, (merizod, testColl) => {
            MongoRunner.stopMongod(merizod);
            const mdbCatalogFile = dbpath + "_mdb_catalog.wt";
            jsTestLog("corrupting catalog file: " + mdbCatalogFile);
            corruptFile(mdbCatalogFile);
        }, "Fatal Assertion 50882");

    /**
     * Test 3. Corrupt the WiredTiger.wt.
     */

    assertErrorOnStartupWhenFilesAreCorruptOrMissing(
        dbpath, baseName, collName, (merizod, testColl) => {
            MongoRunner.stopMongod(merizod);
            const WiredTigerWTFile = dbpath + "WiredTiger.wt";
            jsTestLog("corrupting WiredTiger.wt");
            corruptFile(WiredTigerWTFile);
        }, "Fatal Assertion 50944");

    /**
     * Test 4. Corrupt an index file.
     */

    assertErrorOnRequestWhenFilesAreCorruptOrMissing(
        dbpath,
        baseName,
        collName,
        (merizod, testColl) => {
            const indexName = "a_1";
            assert.commandWorked(testColl.createIndex({a: 1}, {name: indexName}));
            const indexUri = getUriForIndex(testColl, indexName);
            MongoRunner.stopMongod(merizod);
            const indexFile = dbpath + indexUri + ".wt";
            jsTestLog("corrupting index file: " + indexFile);
            corruptFile(indexFile);
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
