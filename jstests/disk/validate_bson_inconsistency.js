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
    testColl.insert({a: 1, b: 2, c: {b: 3}, d: {a: [2, 3, 4], b: {a: 2}}});
    testColl.insert({a: 1, b: 1});

    // Warnings should be triggered iff checkBSONConformance is set to true.
    let res = assert.commandWorked(testColl.validate());
    assert(res.valid, tojson(res));
    assert.eq(res.nNonCompliantDocuments, 0);
    assert.eq(res.warnings.length, 0);

    res = assert.commandWorked(testColl.validate({checkBSONConformance: true}));
    assert(res.valid, tojson(res));
    assert.eq(res.nNonCompliantDocuments, numDocs);
    assert.eq(res.warnings.length, 1);

    MongoRunner.stopMongod(mongod, null, {skipValidation: true});
})();

(function validateDocumentsInvalidUUIDLength() {
    let mongod = startMongodOnExistingPath(dbpath);
    let db = mongod.getDB(baseName);
    const collName = collNamePrefix + count++;
    db.createCollection(collName);
    let coll = db[collName];

    jsTestLog(
        "Checks that warnings are triggered when validating UUIDs that are either too short or too long.");
    coll.insert({u: HexData(4, "deadbeefdeadbeefdeadbeefdeadbeef")});
    coll.insert({u: HexData(4, "deadbeef")});
    coll.insert({
        u: HexData(
            4,
            "deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef")
    });

    let res = coll.validate({checkBSONConformance: true});
    assert(res.valid, tojson(res));
    assert.eq(res.nNonCompliantDocuments, 2);
    assert.eq(res.warnings.length, 1);

    res = coll.validate({checkBSONConformance: false});
    assert(res.valid, tojson(res));
    assert.eq(res.nNonCompliantDocuments, 2);
    assert.eq(res.warnings.length, 1);
    MongoRunner.stopMongod(mongod, null, {skipValidation: true});
})();

(function validateDocumentsInvalidRegexOptions() {
    let mongod = startMongodOnExistingPath(dbpath);
    let db = mongod.getDB(baseName);

    const collName = collNamePrefix + count++;
    db.getCollection(collName).drop();
    assert.commandWorked(db.createCollection(collName));
    let coll = db[collName];

    jsTestLog(
        "Checks that issues are found when we validate regex expressions with invalid options.");
    insertInvalidRegex(coll, mongod, 5);
    mongod = startMongodOnExistingPath(dbpath);
    db = mongod.getDB(baseName);
    coll = db[collName];

    let res = coll.validate({checkBSONConformance: false});
    assert(res.valid, tojson(res));
    assert.eq(res.nNonCompliantDocuments, 5);
    assert.eq(res.warnings.length, 1);

    res = coll.validate({checkBSONConformance: true});
    assert(res.valid, tojson(res));
    assert.eq(res.nNonCompliantDocuments, 5);
    assert.eq(res.warnings.length, 1);

    MongoRunner.stopMongod(mongod, null, {skipValidation: true});
})();

(function validateDocumentsInvalidMD5Length() {
    jsTestLog("Validate document with invalid MD5 length");

    let mongod = startMongodOnExistingPath(dbpath);
    let db = mongod.getDB(baseName);
    const collName = collNamePrefix + count++;

    db.createCollection(collName);
    let testColl = db[collName];
    const properMD5 = HexData(5, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    const improperMD5 = HexData(5, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");

    // Tests that calling validate on a collection with a properly sized md5 doesn't return a
    // warning.
    assert.commandWorked(testColl.insert({"md5Proper": properMD5}));
    let res = assert.commandWorked(testColl.validate());
    assert(res.valid, tojson(res));
    assert.eq(res.nNonCompliantDocuments, 0);
    assert.eq(res.warnings.length, 0);

    // Tests that calling validate on a collection with an improperly sized md5 returns a
    // warning.
    assert.commandWorked(testColl.insert({"md5Improper": improperMD5}));
    res = assert.commandWorked(testColl.validate());
    assert(res.valid, tojson(res));
    assert.eq(res.nNonCompliantDocuments, 1);
    assert.eq(res.warnings.length, 1);

    // Tests that calling validate, with BSONConsistencyCheck true, on a collection with an
    // improperly sized md5 returns a warning.
    assert.commandWorked(testColl.insert({"md5ImproperBSONConsistencyCheck": improperMD5}));
    res = assert.commandWorked(testColl.validate({checkBSONConformance: true}));
    assert(res.valid, tojson(res));
    assert.eq(res.nNonCompliantDocuments, 2);

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

    res = assert.commandWorked(testColl.validate({checkBSONConformance: true}));
    assert(res.valid, tojson(res));
    assert.eq(res.nNonCompliantDocuments, 7);
    assert.eq(res.warnings.length, 1);

    MongoRunner.stopMongod(mongod, null, {skipValidation: true});
})();

(function validateDocumentsCorruptedBinDataColumn() {
    jsTestLog("Validate documents with corrupted or misformed BinData Columns.");

    let mongod = startMongodOnExistingPath(dbpath);
    let db = mongod.getDB(baseName);
    const collName = collNamePrefix + count++;
    db.createCollection(collName);
    let testColl = db[collName];

    // Inserts a rubbish (random string) BSON Column.
    testColl.insert({a: BinData(7, "O2FkZmdqYWtsamhnJ2xhamhkZzthaCdmZGphZ2hkYQ==")});
    // Inserts one valid BSON Column to check that it doesn't cause a false positive.
    testColl.insert(
        {a: BinData(7, "AQAAAAAAAAAAQJN/AAAAAAAAAAIAAAAAAAAABwAAAAAAAAAOAAAAAAAAAAA=")});

    // Calling validate without 'checkBSONConformance' should not return any warnings.
    let res = assert.commandWorked(testColl.validate());
    assert(res.valid, tojson(res));
    assert.eq(res.warnings.length, 0);
    assert.eq(res.nNonCompliantDocuments, 0);

    res = assert.commandWorked(testColl.validate({checkBSONConformance: true}));
    assert(res.valid, tojson(res));
    assert.eq(res.warnings.length, 1);
    assert.eq(res.nNonCompliantDocuments, 1);

    MongoRunner.stopMongod(mongod, null, {skipValidation: true});
})();

(function validateDocumentsNonSequentialArrayIndexes() {
    jsTestLog("Validate documents with array indices that are not sequential");

    let mongod = startMongodOnExistingPath(dbpath);
    let db = mongod.getDB(baseName);
    const collName = collNamePrefix + count++;
    db.createCollection(collName);
    let testColl = db[collName];

    let uri = getUriForColl(testColl);
    const numDocs = 10;
    insertNonSequentialArrayIndexes(testColl, uri, mongod, numDocs);

    mongod = startMongodOnExistingPath(dbpath);
    db = mongod.getDB(baseName);
    testColl = db[collName];

    res = assert.commandWorked(testColl.validate());
    assert(res.valid, tojson(res));
    assert.eq(res.nNonCompliantDocuments, 10);
    assert.eq(res.warnings.length, 1);

    res = assert.commandWorked(testColl.validate({checkBSONConformance: true}));
    assert(res.valid, tojson(res));
    assert.eq(res.nNonCompliantDocuments, 10);
    assert.eq(res.warnings.length, 1);

    MongoRunner.stopMongod(mongod, null, {skipValidation: true});
})();

(function validateDocumentsInvalidUTF8() {
    jsTestLog("Validate documents with invalid UTF-8 strings");

    let mongod = startMongodOnExistingPath(dbpath);
    let db = mongod.getDB(baseName);
    const collName = collNamePrefix + count++;
    db.createCollection(collName);
    let testColl = db[collName];

    let uri = getUriForColl(testColl);
    const numDocs = 10;
    insertInvalidUTF8(testColl, uri, mongod, numDocs);

    mongod = startMongodOnExistingPath(dbpath);
    db = mongod.getDB(baseName);
    testColl = db[collName];

    res = assert.commandWorked(testColl.validate());
    assert(res.valid, tojson(res));
    assert.eq(res.nNonCompliantDocuments, 0);
    assert.eq(res.warnings.length, 0);

    res = assert.commandWorked(testColl.validate({checkBSONConformance: true}));
    assert(res.valid, tojson(res));
    assert.eq(res.nNonCompliantDocuments, 10);
    assert.eq(res.warnings.length, 1);

    MongoRunner.stopMongod(mongod, null, {skipValidation: true});
})();

(function validateDocumentsInvalidEncryptedBSONValue() {
    jsTestLog("Validate documents with invalid Encrypted BSON Value");

    let mongod = startMongodOnExistingPath(dbpath);
    let db = mongod.getDB(baseName);
    const collName = collNamePrefix + count++;

    db.createCollection(collName);
    let testColl = db[collName];
    // A valid Encrypted BSON document with the type byte, 16-byte key uuid, original BSON type
    // byte, and an empty cipher text.
    const properFLE = HexData(6, "060102030405060708091011121314151610");
    // Invalid Encrypted BSON Value subtype 3.
    const improperFLE1 = HexData(6, "030102030405060708091011121314151610");
    // Invalid original BSON type MinKey.
    const improperFLE2 = HexData(6, "0601020304050607080910111213141516ff");
    // Empty Encrypted BSON Value.
    const improperFLE3 = HexData(6, "");
    // Short Encrypted BSON Value.
    const improperFLE4 = HexData(6, "0601");

    assert.commandWorked(testColl.insertMany([
        {"fle": properFLE},
        {"fle": improperFLE1},
        {"fle": improperFLE2},
        {"fle": improperFLE3},
        {"fle": improperFLE4},
    ]));

    let res = assert.commandWorked(testColl.validate());
    assert(res.valid, tojson(res));
    assert.eq(res.nNonCompliantDocuments, 4);
    assert.eq(res.warnings.length, 1);

    res = assert.commandWorked(testColl.validate({checkBSONConformance: true}));
    assert(res.valid, tojson(res));
    assert.eq(res.nNonCompliantDocuments, 4);
    assert.eq(res.warnings.length, 1);

    MongoRunner.stopMongod(mongod, null, {skipValidation: true});
})();
})();
