/**
 * Tests that --repair on WiredTiger correctly and gracefully handles inconsistent indexes.
 *
 * @tags: [requires_wiredtiger]
 */

(function() {

load('jstests/disk/libs/wt_file_helper.js');

const baseName = "wt_repair_inconsistent_index";
const collName = "test";
const dbpath = MongoRunner.dataPath + baseName + "/";

/**
 * Run the test by supplying additional paramters to MongoRunner.runMongod with 'mongodOptions'.
 */
let runTest = function(mongodOptions) {
    resetDbpath(dbpath);
    jsTestLog("Running test with args: " + tojson(mongodOptions));

    /**
     * Test 1. Configure the skipIndexNewRecords failpoint, then insert documents into
     * testColl, which will result in an index inconsistency. Run repair and verify
     * that the index is rebuilt.
     */

    let mongod = startMongodOnExistingPath(dbpath, mongodOptions);
    let testColl = mongod.getDB(baseName)[collName];

    const doc = {a: 1};
    assert.commandWorked(testColl.insert(doc));

    const indexName = "a_1";
    assert.commandWorked(testColl.createIndex({a: 1}, {name: indexName}));
    assertQueryUsesIndex(testColl, doc, indexName);

    let testCollUri = getUriForColl(testColl);
    let indexUri = getUriForIndex(testColl, indexName);

    let db = mongod.getDB(baseName);
    assert.commandWorked(
        db.adminCommand({configureFailPoint: 'skipIndexNewRecords', mode: 'alwaysOn'}));
    assert.commandWorked(testColl.insert({a: 2}));

    // Disable validation because it is expected to not pass due to index inconsistencies.
    MongoRunner.stopMongod(mongod, null, {skipValidation: true});

    assertRepairSucceeds(dbpath, mongod.port, mongodOptions);
    mongod = startMongodOnExistingPath(dbpath, mongodOptions);
    testColl = mongod.getDB(baseName)[collName];

    // Repair doesn't create new idents because validate repair mode fixed index inconsistencies.
    assert.eq(indexUri, getUriForIndex(testColl, indexName));

    assertQueryUsesIndex(testColl, doc, indexName);
    assert.eq(testCollUri, getUriForColl(testColl));
    assert.eq(testColl.count(), 2);

    MongoRunner.stopMongod(mongod);
};

runTest({});
runTest({directoryperdb: ""});
runTest({wiredTigerDirectoryForIndexes: ""});
})();
