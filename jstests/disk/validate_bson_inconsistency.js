/**
 * Tests that the validate command detects various types of BSON inconsistencies.
 *
 * @tags: [featureFlagExtendValidateCommand]
 */

(function() {

load('jstests/disk/libs/wt_file_helper.js');

const baseName = "validate_bson_inconsistency";
const collNamePrefix = "test_";
let count = 0;
const dbpath = MongoRunner.dataPath + baseName + "/";

resetDbpath(dbpath);

(function validateDocumentsDuplicateFieldNames() {
    jsTestLog("Validate documents with duplicate field names");

    let mongod = startMongodOnExistingPath(dbpath);
    let db = mongod.getDB(baseName);
    const collName = collNamePrefix + count++;
    db.createCollection(collName);
    let testColl = db[collName];

    let uri = getUriForColl(testColl);
    const numDocs = 10;
    insertDocDuplicateFieldName(testColl, uri, mongod, numDocs);

    mongod = startMongodOnExistingPath(dbpath);
    db = mongod.getDB(baseName);
    testColl = db[collName];

    let res = assert.commandWorked(testColl.validate());
    assert(res.valid, tojson(res));
    // TODO: Check the warnings that the documents with duplicate field names are detected.

    MongoRunner.stopMongod(mongod, null, {skipValidation: true});
})();

(function validateDocumentsDeprecatedTypes() {
    jsTestLog("Validate documents with deprecated types");

    let mongod = startMongodOnExistingPath(dbpath);
    let db = mongod.getDB(baseName);
    const collName = collNamePrefix + count++;
    db.createCollection(collName);
    let testColl = db[collName];

    let uri = getUriForColl(testColl);
    const numDocs = 1;
    insertDocSymbolField(testColl, uri, mongod, numDocs);

    mongod = startMongodOnExistingPath(dbpath);
    db = mongod.getDB(baseName);
    testColl = db[collName];

    assert.commandWorked(testColl.insert({a: undefined}));
    assert.commandWorked(
        testColl.insert({b: DBPointer("db", new ObjectId("dbdbdbdbdbdbdbdbdbdbdbdb"))}));
    assert.commandWorked(testColl.insert({c: Code("function(){return 1;}", {})}));
    assert.commandWorked(testColl.insert(
        {d: BinData(2, "KwAAAFRoZSBxdWljayBicm93biBmb3gganVtcHMgb3ZlciB0aGUgbGF6eSBkb2c=")}));
    assert.commandWorked(testColl.insert({e: BinData(3, "000102030405060708090a0b0c0d0e0f")}));
    assert.commandWorked(testColl.insert({
        a: undefined,
        b: DBPointer("db", new ObjectId("dbdbdbdbdbdbdbdbdbdbdbdb")),
        c: Code("function(){return 1;}", {}),
        d: BinData(2, "KwAAAFRoZSBxdWljayBicm93biBmb3gganVtcHMgb3ZlciB0aGUgbGF6eSBkb2c="),
        e: BinData(3, "000102030405060708090a0b0c0d0e0f")
    }));

    let res = assert.commandWorked(testColl.validate());
    assert(res.valid, tojson(res));
    assert.eq(res.nNonCompliantDocuments, 7);
    assert.eq(res.warnings.length, 1);

    res = assert.commandWorked(testColl.validate({checkBSONConsistency: true}));
    assert(res.valid, tojson(res));
    assert.eq(res.nNonCompliantDocuments, 7);
    assert.eq(res.warnings.length, 1);

    MongoRunner.stopMongod(mongod, null, {skipValidation: true});
})();
})();
