/**
 * Test BSON validation warning logs in the dbCheck command for full bson validate mode.
 *
 * @tags: [
 *   featureFlagSecondaryIndexChecksInDbCheck
 * ]
 */

import {
    getUriForColl,
    insertInvalidUTF8,
    startMongodOnExistingPath,
} from "jstests/disk/libs/wt_file_helper.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {clearHealthLog, runDbCheck} from "jstests/replsets/libs/dbcheck_utils.js";

const BSONWarningQuery = {
    operation: "dbCheckBatch",
    severity: "warning",
    msg: "Document is not well-formed BSON"
};

const baseName = "dbcheck_full_bson_validate";
const collNamePrefix = "test_";
const dbpath = MongoRunner.dataPath + baseName + "/";

const mongod = startMongodOnExistingPath(dbpath);
let db = mongod.getDB(baseName);
const collName = collNamePrefix;
db.createCollection(collName);
const testColl = db[collName];

const uri = getUriForColl(testColl);
const numDocs = 1;
insertInvalidUTF8(testColl, uri, mongod, numDocs);

// Create a replica set from standalone node and add a secondary.
const replSet = new ReplSetTest({name: jsTestName(), nodes: 2});
replSet.start(0, {dbpath: dbpath, noCleanData: true});
replSet.start(1);
replSet.initiate();

const primary = replSet.getPrimary();
const secondary = replSet.getSecondary();
db = primary.getDB(baseName);
const primaryHealthlog = primary.getDB("local").system.healthlog;
const secondaryHealthlog = secondary.getDB("local").system.healthlog;
clearHealthLog(replSet);

let dbCheckParameters = {
    validateMode: "dataConsistencyAndMissingIndexKeysCheck",
    bsonValidateMode: "kFull"
};
runDbCheck(replSet, db, collName, dbCheckParameters, true /* awaitCompletion */);

assert.soon(function() {
    if (primaryHealthlog.find(BSONWarningQuery).itcount() == numDocs) {
        return String(primaryHealthlog.find(BSONWarningQuery)
                          .toArray()
                          .map((a => a["data"]["context"]["recordID"]))) == String([1]);
    }
    return false;
}, "dbCheck command didn't complete, record ID of invalid BSON document not in health log entry");

assert.soon(function() {
    if (secondaryHealthlog.find(BSONWarningQuery).itcount() == numDocs) {
        return String(secondaryHealthlog.find(BSONWarningQuery)
                          .toArray()
                          .map((a => a["data"]["context"]["recordID"]))) == String([1]);
    }
    return false;
}, "dbCheck command didn't complete, record ID of invalid BSON document not in health log entry");

replSet.stopSet();
