/**
 * Test secondaryIndexCheckParameter field in dbCheck health log entries.
 *
 * @tags: [
 *   requires_fcv_80
 * ]
 */

import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {checkHealthLog, resetAndInsert, runDbCheck} from "jstests/replsets/libs/dbcheck_utils.js";

const dbName = "dbCheckParametersInHealthLog";
const colName = "dbCheckParametersInHealthLog-collection";

const replSet = new ReplSetTest({
    name: jsTestName(),
    nodes: 2,
});
replSet.startSet();
replSet.initiateWithHighElectionTimeout();

const primary = replSet.getPrimary();
const secondary = replSet.getSecondary();
const primaryHealthlog = primary.getDB("local").system.healthlog;
const secondaryHealthlog = secondary.getDB("local").system.healthlog;
const db = primary.getDB(dbName);
const nDocs = 1000;
const maxDocsPerBatch = 100;
const writeConcern = {
    w: 'majority'
};

function testParametersInHealthlog() {
    jsTestLog("Testing that health log contains validateMode parameters.");

    // The start and stop health log entries on the primary and secondary nodes should contain the
    // default validateMode parameter dataConsistency and the default bsonValidateMode parameter
    // kDefault when no bsonValidateMode parameter was passed in (so the default is `kDefault`).
    resetAndInsert(replSet, db, colName, nDocs);
    let dbCheckParameters = {maxDocsPerBatch: maxDocsPerBatch, batchWriteConcern: writeConcern};
    runDbCheck(replSet, db, colName, dbCheckParameters);
    let query = {
        namespace: dbName + "." + colName,
        collectionUUID: getUUIDFromListCollections(db, colName),
        data: {
            dbCheckParameters: {
                validateMode: "dataConsistency",
                secondaryIndex: "",
                skipLookupForExtraKeys: false,
                bsonValidateMode: "kDefault"
            }
        }
    };
    checkHealthLog(primaryHealthlog, query, 2);
    checkHealthLog(secondaryHealthlog, query, 2);

    // The start and stop health log entries on the primary and secondary nodes should contain the
    // validateMode and bsonValidateMode parameters that are passed in.
    resetAndInsert(replSet, db, colName, nDocs);
    dbCheckParameters = {
        maxDocsPerBatch: maxDocsPerBatch,
        batchWriteConcern: writeConcern,
        validateMode: "dataConsistencyAndMissingIndexKeysCheck",
        bsonValidateMode: "kExtended"
    };
    runDbCheck(replSet, db, colName, dbCheckParameters);
    query = {
        namespace: dbName + "." + colName,
        collectionUUID: getUUIDFromListCollections(db, colName),
        data: {
            dbCheckParameters: {
                validateMode: "dataConsistencyAndMissingIndexKeysCheck",
                secondaryIndex: "",
                skipLookupForExtraKeys: false,
                bsonValidateMode: "kExtended"
            }
        }
    };
    checkHealthLog(primaryHealthlog, query, 2);
    checkHealthLog(secondaryHealthlog, query, 2);

    // The start and stop health log entries on the primary and secondary nodes should contain the
    // secondaryIndex parameter when validateMode is extraIndexKeysCheck and the default
    // bsonValidateMode parameter kDefault when no bsonValidateMode parameter was passed in (so the
    // default is `kDefault`).
    resetAndInsert(replSet, db, colName, nDocs);
    dbCheckParameters = {
        maxDocsPerBatch: maxDocsPerBatch,
        batchWriteConcern: writeConcern,
        validateMode: "extraIndexKeysCheck",
        secondaryIndex: "secondaryIndex",
        skipLookupForExtraKeys: true
    };
    runDbCheck(replSet, db, colName, dbCheckParameters);
    query = {
        namespace: dbName + "." + colName,
        collectionUUID: getUUIDFromListCollections(db, colName),
        data: {
            dbCheckParameters: {
                validateMode: "extraIndexKeysCheck",
                secondaryIndex: "secondaryIndex",
                skipLookupForExtraKeys: true,
                bsonValidateMode: "kDefault"
            }
        }
    };
    checkHealthLog(primaryHealthlog, query, 2);
    checkHealthLog(secondaryHealthlog, query, 2);
}

testParametersInHealthlog();

replSet.stopSet();
