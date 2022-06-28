/**
 * Tests that the validate command detects various types of BSON inconsistencies.
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
})();
