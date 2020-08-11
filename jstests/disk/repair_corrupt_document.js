/**
 * Tests that --repair deletes corrupt BSON documents.
 */

(function() {

load('jstests/disk/libs/wt_file_helper.js');

if (_isWindows()) {
    // TODO(SERVER-50205): Re-enable under Windows.
    jsTestLog('Skipping test under Windows.');
    return;
}

const baseName = "repair_corrupt_document";
const collName = "test";
const dbpath = MongoRunner.dataPath + baseName + "/";
const doc1 = {
    a: 1
};
const doc2 = {
    a: 2
};
const indexName = "a_1";

let indexUri;
let validDoc;

resetDbpath(dbpath);
let port;

// Initialize test collection.
let createCollWithDoc = function(coll) {
    assert.commandWorked(coll.insert(doc1));
    validDoc = coll.findOne(doc1);

    assert.commandWorked(coll.createIndex({a: 1}, {name: indexName}));
    assertQueryUsesIndex(coll, doc1, indexName);
    indexUri = getUriForIndex(coll, indexName);
    return coll;
};

// Insert corrupt document for testing via failpoint.
let corruptDocumentOnInsert = function(db, coll) {
    jsTestLog("Corrupt document BSON on insert.");
    assert.commandWorked(
        db.adminCommand({configureFailPoint: "corruptDocumentOnInsert", mode: "alwaysOn"}));
    assert.commandWorked(coll.insert(doc2));
    assert.commandWorked(
        db.adminCommand({configureFailPoint: "corruptDocumentOnInsert", mode: "off"}));
};

/**
 * Test 1: Insert corrupt document and verify results are valid without rebuilding indexes after
 * running repair.
 */
(function startStandaloneWithCorruptDoc() {
    jsTestLog("Entering startStandaloneWithCorruptDoc...");

    let mongod = startMongodOnExistingPath(dbpath);
    port = mongod.port;
    let db = mongod.getDB(baseName);
    let testColl = db[collName];

    testColl = createCollWithDoc(testColl);
    corruptDocumentOnInsert(db, testColl);

    assert.eq(testColl.count(), 2);

    MongoRunner.stopMongod(mongod, null, {skipValidation: true});
    jsTestLog("Exiting startStandaloneWithCorruptDoc.");
})();

// Run validate with repair mode and verify corrupt document is removed from collection.
(function runRepairAndVerifyCollectionDocs() {
    jsTestLog("Entering runRepairAndVerifyCollectionDocs...");

    assertRepairSucceeds(dbpath, port, {});

    let mongod = startMongodOnExistingPath(dbpath);
    let testColl = mongod.getDB(baseName)[collName];

    // Repair removed the corrupt document.
    assert.eq(testColl.count(), 1);
    assert.eq(testColl.findOne(doc1), validDoc);

    // Repair did not need to rebuild indexes.
    assert.eq(indexUri, getUriForIndex(testColl, indexName));

    MongoRunner.stopMongod(mongod);
    jsTestLog("Exiting runValidateWithRepairMode.");
})();
})();
