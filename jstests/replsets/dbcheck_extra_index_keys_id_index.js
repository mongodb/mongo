/**
 *  Test dbcheck ExtraIndexKeys on _id index. The check should work on non-clustered collections
 *  but fail on clustered collections with proper message.
 *
 * @tags: [
 *   featureFlagSecondaryIndexChecksInDbCheck
 * ]
 */

import {
    checkHealthLog,
    clearHealthLog,
    logQueries,
    runDbCheck
} from "jstests/replsets/libs/dbcheck_utils.js";

// Skipping data consistency checks because data is inserted into primary and secondary separately.
TestData.skipCheckDBHashes = true;

const dbName = "dbCheckClusteredCollections";
const collName = "dbCheckClusteredCollectionsColl";

const replSet = new ReplSetTest({name: jsTestName(), nodes: 2});
replSet.startSet();
replSet.initiateWithHighElectionTimeout();

const primary = replSet.getPrimary();
const primaryDb = primary.getDB(dbName);
const secondary = replSet.getSecondary();
const primaryHealthLog = primary.getDB("local").system.healthlog;
const secondaryHealthLog = secondary.getDB("local").system.healthlog;

const doc = {
    a: 1
};
const maxDocsPerBatch = 10000;

function testClusteredCollection() {
    jsTestLog(
        "Run dbcheck ExtraIndexKeys on _id index of clustered collection should raise error with appropriate information.");
    clearHealthLog(replSet);
    primaryDb[collName].drop();

    assert.commandWorked(
        primaryDb.createCollection(collName, {clusteredIndex: {key: {_id: 1}, unique: true}}));
    assert.commandWorked(primaryDb[collName].insert(doc));

    runDbCheck(replSet, primary.getDB(dbName), collName, {
        maxDocsPerBatch: maxDocsPerBatch,
        validateMode: "extraIndexKeysCheck",
        secondaryIndex: "_id_",
    });

    const nWarnings = 1;
    checkHealthLog(
        primaryHealthLog, logQueries.checkIdIndexOnClusteredCollectionWarningQuery, nWarnings);

    checkHealthLog(primaryHealthLog, logQueries.allErrorsOrWarningsQuery, nWarnings);
    checkHealthLog(secondaryHealthLog, logQueries.allErrorsOrWarningsQuery, 0);
}

function testNonClusteredCollection() {
    jsTestLog(
        "Run dbcheck ExtraIndexKeys on _id index of non-clustered collection should succeed.");
    clearHealthLog(replSet);
    primaryDb[collName].drop();

    assert.commandWorked(primaryDb.createCollection(collName));
    assert.commandWorked(primaryDb[collName].insert(doc));

    runDbCheck(replSet, primary.getDB(dbName), collName, {
        maxDocsPerBatch: maxDocsPerBatch,
        validateMode: "extraIndexKeysCheck",
        secondaryIndex: "_id_",
    });

    // Confirm there is no error or warnings
    checkHealthLog(primaryHealthLog, logQueries.allErrorsOrWarningsQuery, 0);
    checkHealthLog(secondaryHealthLog, logQueries.allErrorsOrWarningsQuery, 0);
}

testClusteredCollection();
testNonClusteredCollection();

replSet.stopSet();
