/**
 * Tests that --repair deletes documents containing duplicate unique keys and inserts them into a
 * local lost and found collection.
 *
 * @tags: [requires_wiredtiger]
 */

(function() {

load('jstests/disk/libs/wt_file_helper.js');
load("jstests/libs/uuid_util.js");

const baseName = "repair_duplicate_keys";
const localBaseName = "local";
const collName = "test";
const lostAndFoundCollBaseName = "system.lost_and_found.";

const dbpath = MongoRunner.dataPath + baseName + "/";
const indexName = "a_1";
const doc1 = {
    a: 1,
};
const doc2 = {
    a: 10,
};
const doc3 = {
    a: 100,
};
const docWithId = {
    a: 1,
    _id: 1
};
const dupDocWithId = {
    a: 1,
    _id: 1
};

resetDbpath(dbpath);
let port;

// Initializes test collection for tests 1-3.
let createIndexedCollWithDocs = function(coll) {
    assert.commandWorked(coll.insert(doc1));
    assert.commandWorked(coll.createIndex({a: 1}, {name: indexName, unique: true}));
    assert.commandWorked(coll.insert(doc2));
    assert.commandWorked(coll.insert(doc3));

    assertQueryUsesIndex(coll, doc1, indexName);
    assertQueryUsesIndex(coll, doc2, indexName);
    assertQueryUsesIndex(coll, doc3, indexName);
    return coll;
};

// Bypasses DuplicateKey insertion error for testing via failpoint.
let addDuplicateDocumentToCol = function(db, coll, doc) {
    jsTestLog("Insert document without index entries.");
    assert.commandWorked(
        db.adminCommand({configureFailPoint: "skipIndexNewRecords", mode: "alwaysOn"}));

    assert.commandWorked(coll.insert(doc));

    assert.commandWorked(db.adminCommand({configureFailPoint: "skipIndexNewRecords", mode: "off"}));
};

// Runs repair on collection with possible duplicate keys and verifies original documents in
// collection initialized with "createIndexedCollWithDocs" are still present.
let runRepairAndVerifyCollectionDocs = function() {
    jsTestLog("Entering runRepairAndVerifyCollectionDocs...");

    assertRepairSucceeds(dbpath, port);

    let mongod = startMongodOnExistingPath(dbpath);
    let testColl = mongod.getDB(baseName)[collName];

    assert.eq(testColl.find(doc1).itcount(), 1);
    assert.eq(testColl.find(doc2).itcount(), 1);
    assert.eq(testColl.find(doc3).itcount(), 1);
    assert.eq(testColl.count(), 3);

    MongoRunner.stopMongod(mongod);
    jsTestLog("Exiting runRepairAndVerifyCollectionDocs.");
};

/* Test 1: Insert unique documents and verify that no local database is generated. */
(function startStandaloneWithNoDup() {
    jsTestLog("Entering startStandaloneWithNoDup...");

    let mongod = startMongodOnExistingPath(dbpath);
    port = mongod.port;
    let testColl = mongod.getDB(baseName)[collName];

    testColl = createIndexedCollWithDocs(testColl);
    assert.commandFailedWithCode(testColl.insert(doc1), [ErrorCodes.DuplicateKey]);
    assert.commandFailedWithCode(testColl.insert(doc2), [ErrorCodes.DuplicateKey]);
    assert.commandFailedWithCode(testColl.insert(doc3), [ErrorCodes.DuplicateKey]);

    assert.eq(testColl.count(), 3);

    MongoRunner.stopMongod(mongod);
    jsTestLog("Exiting startStandaloneWithNoDup.");
})();

runRepairAndVerifyCollectionDocs();

(function checkLostAndFoundCollForNoDup() {
    jsTestLog("Entering checkLostAndFoundCollForNoDup...");

    let mongod = startMongodOnExistingPath(dbpath);
    const uuid_obj = getUUIDFromListCollections(mongod.getDB(baseName), collName);
    const uuid = extractUUIDFromObject(uuid_obj);

    let localColl = mongod.getDB(localBaseName)[lostAndFoundCollBaseName + uuid];
    assert.isnull(localColl.exists());

    MongoRunner.stopMongod(mongod);
    jsTestLog("Exiting checkLostAndFoundCollForNoDup.");
})();

/* Test 2: Insert one duplicate document into test collection and verify that repair deletes the
 * document from the collection, generates a "local.lost_and_found" collection and inserts
 * duplicate document into it. */
(function startStandaloneWithOneDup() {
    jsTestLog("Entering startStandaloneWithOneDup...");
    resetDbpath(dbpath);

    let mongod = startMongodOnExistingPath(dbpath);
    port = mongod.port;
    let db = mongod.getDB(baseName);
    let testColl = mongod.getDB(baseName)[collName];

    testColl = createIndexedCollWithDocs(testColl);
    assert.commandFailedWithCode(testColl.insert(doc1), [ErrorCodes.DuplicateKey]);

    addDuplicateDocumentToCol(db, testColl, doc1);

    assert.eq(testColl.count(), 4);

    MongoRunner.stopMongod(mongod, undefined, {skipValidation: true});
    jsTestLog("Exiting startStandaloneWithOneDup.");
})();

runRepairAndVerifyCollectionDocs();

(function checkLostAndFoundCollForOneDup() {
    jsTestLog("Entering checkLostAndFoundCollForOneDup...");

    let mongod = startMongodOnExistingPath(dbpath);
    const uuid_obj = getUUIDFromListCollections(mongod.getDB(baseName), collName);
    const uuid = extractUUIDFromObject(uuid_obj);

    let localColl = mongod.getDB(localBaseName)[lostAndFoundCollBaseName + uuid];
    assert.eq(localColl.find(doc1).itcount(), 1);
    assert.eq(localColl.count(), 1);

    MongoRunner.stopMongod(mongod);
    jsTestLog("Exiting checkLostAndFoundCollForOneDup.");
})();

/* Test 3: Insert multiple duplicate documents into the test collection and verify that repair
 * deletes the documents from the collection, generates a "local.lost_and_found" collection and
 * inserts duplicate document into it.
 */
(function startStandaloneWithMultipleDups() {
    jsTestLog("Entering startStandaloneWithMultipleDups...");
    resetDbpath(dbpath);

    let mongod = startMongodOnExistingPath(dbpath);
    port = mongod.port;
    let db = mongod.getDB(baseName);
    let testColl = mongod.getDB(baseName)[collName];

    testColl = createIndexedCollWithDocs(testColl);
    assert.commandFailedWithCode(testColl.insert(doc1), [ErrorCodes.DuplicateKey]);

    addDuplicateDocumentToCol(db, testColl, doc1);
    addDuplicateDocumentToCol(db, testColl, doc2);
    addDuplicateDocumentToCol(db, testColl, doc3);

    assert.eq(testColl.count(), 6);

    MongoRunner.stopMongod(mongod, undefined, {skipValidation: true});
    jsTestLog("Exiting startStandaloneWithMultipleDups.");
})();

runRepairAndVerifyCollectionDocs();

(function checkLostAndFoundCollForDups() {
    jsTestLog("Entering checkLostAndFoundCollForDups...");

    let mongod = startMongodOnExistingPath(dbpath);
    const uuid_obj = getUUIDFromListCollections(mongod.getDB(baseName), collName);
    const uuid = extractUUIDFromObject(uuid_obj);

    let localColl = mongod.getDB(localBaseName)[lostAndFoundCollBaseName + uuid];

    assert.eq(localColl.find(doc1).itcount(), 1);
    assert.eq(localColl.find(doc2).itcount(), 1);
    assert.eq(localColl.find(doc3).itcount(), 1);
    assert.eq(localColl.count(), 3);

    MongoRunner.stopMongod(mongod);
    jsTestLog("Exiting checkLostAndFoundCollForDups.");
})();

/* Test 4: Insert document with both duplicate "a" field and duplicate _id field. Verify that repair
 * deletes the duplicate document from test collection, generates a "local.lost_and_found"
 * collection and inserts duplicate document into it.
 */
(function startStandaloneWithDoubleDup() {
    jsTestLog("Entering startStandaloneWithDoubleDup...");
    resetDbpath(dbpath);

    let mongod = startMongodOnExistingPath(dbpath);
    port = mongod.port;
    let db = mongod.getDB(baseName);
    let testColl = mongod.getDB(baseName)[collName];

    assert.commandWorked(testColl.insert(docWithId));
    assert.commandWorked(testColl.createIndex({a: 1}, {name: indexName, unique: true}));

    addDuplicateDocumentToCol(db, testColl, dupDocWithId);

    assert.eq(testColl.count(), 2);

    MongoRunner.stopMongod(mongod, undefined, {skipValidation: true});
    jsTestLog("Exiting startStandaloneWithDoubleDup.");
})();

(function runRepairAndVerifyCollectionDoc() {
    jsTestLog("Running repair...");

    assertRepairSucceeds(dbpath, port);

    let mongod = startMongodOnExistingPath(dbpath);
    let testColl = mongod.getDB(baseName)[collName];

    assert.eq(testColl.find(docWithId).itcount(), 1);
    assert.eq(testColl.count(), 1);

    MongoRunner.stopMongod(mongod);

    jsTestLog("Finished repairing.");
})();

(function checkLostAndFoundCollForDoubleDup() {
    jsTestLog("Entering checkLostAndFoundCollForDoubleDup...");

    let mongod = startMongodOnExistingPath(dbpath);
    const uuid_obj = getUUIDFromListCollections(mongod.getDB(baseName), collName);
    const uuid = extractUUIDFromObject(uuid_obj);

    let localColl = mongod.getDB(localBaseName)[lostAndFoundCollBaseName + uuid];
    assert.eq(localColl.find(docWithId).itcount(), 1);
    assert.eq(localColl.count(), 1);

    MongoRunner.stopMongod(mongod);
    jsTestLog("Exiting checkLostAndFoundCollForDoubleDup.");
})();
})();
