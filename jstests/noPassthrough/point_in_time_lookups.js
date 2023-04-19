/**
 * Tests the expected point-in-time lookup behaviour.
 *
 * @tags: [
 *     requires_persistence,
 *     requires_replication,
 *     requires_fcv_70,
 * ]
 */
(function() {
"use strict";

const replTest = new ReplSetTest({
    nodes: 1,
    nodeOptions: {
        setParameter: {
            // Set the history window to 1 hour to prevent the oldest timestamp from advancing. This
            // is necessary to avoid removing data files across restarts for this test.
            minSnapshotHistoryWindowInSeconds: 60 * 60,
            // Exercise yielding and restoring point-in-time collections.
            internalQueryExecYieldIterations: 0,
            internalQueryExecYieldPeriodMS: 0
        }
    }
});

replTest.startSet();
replTest.initiate();

const restart = function() {
    replTest.stop(0, 9, {allowedExitCode: MongoRunner.EXIT_SIGKILL}, {forRestart: true});
    replTest.start(
        0,
        {
            setParameter: {
                // Set the history window to 1 hour to prevent the oldest timestamp from advancing.
                // This is necessary to avoid removing data files across restarts for this test.
                minSnapshotHistoryWindowInSeconds: 60 * 60,
            }
        },
        true /* restart */);
};

const primary = function() {
    return replTest.getPrimary();
};

const db = function() {
    return primary().getDB("test");
};

const coll = function() {
    return db()[jsTestName()];
};

const insert = function(document) {
    // The write concern guarantees the stable and oldest timestamp are bumped.
    return assert
        .commandWorked(db().runCommand(
            {insert: coll().getName(), documents: [document], writeConcern: {w: "majority"}}))
        .operationTime;
};

const find = function(collName, withIndex, atClusterTime, numDocs, expectedError) {
    let res = {};
    if (withIndex) {
        res = db().runCommand({
            find: collName,
            hint: {x: 1},
            readConcern: {level: "snapshot", atClusterTime: atClusterTime}
        });
    } else {
        res = db().runCommand(
            {find: collName, readConcern: {level: "snapshot", atClusterTime: atClusterTime}});
    }

    if (expectedError) {
        assert.commandFailedWithCode(res, ErrorCodes.BadValue);
    } else {
        assert.commandWorked(res);
        assert.eq(numDocs, res.cursor.firstBatch.length);
    }
};

// Insert a document in an unused collection to obtain a timestamp before creating the collection
// used for the test.
const oldestTS = assert.commandWorked(db().createCollection("unused")).operationTime;
jsTestLog("Oldest timestamp: " + tojson(oldestTS));

const createTS = assert.commandWorked(db().createCollection(coll().getName())).operationTime;
jsTestLog("Create timestamp: " + tojson(createTS));

const firstInsertTS = insert({x: 1});
jsTestLog("First insert timestamp: " + tojson(firstInsertTS));

const indexTS = assert.commandWorked(coll().createIndex({x: 1})).operationTime;
jsTestLog("Index timestamp: " + tojson(indexTS));

const secondInsertTS = insert({x: 1});
jsTestLog("Second insert timestamp: " + tojson(secondInsertTS));

const renameTS = assert.commandWorked(coll().renameCollection("renamed")).operationTime;
jsTestLog("Rename timestamp: " + tojson(renameTS));

const dropTS = assert.commandWorked(db().runCommand({drop: "renamed"})).operationTime;
jsTestLog("Drop timestamp: " + tojson(dropTS));

const recreatedTS = assert.commandWorked(db().createCollection(coll().getName())).operationTime;
jsTestLog("Recreated timestamp: " + tojson(recreatedTS));

const thirdInsertTS = insert({x: 1});
jsTestLog("Third insert timestamp: " + tojson(thirdInsertTS));

// Create an index and drop it immediately.
const createIndexTS = assert.commandWorked(coll().createIndex({x: 1})).operationTime;
const dropIndexTS = assert.commandWorked(coll().dropIndex({x: 1})).operationTime;

const runPointInTimeTests = function() {
    (function oldestTimestampTests() {
        // Point-in-time reads on a collection before it was created should have like reading from a
        // non-existent collection.
        find(coll().getName(), /*withIndex=*/ false, oldestTS, /*numDocs=*/ 0);
        find(coll().getName(), /*withIndex=*/ true, oldestTS, /*numDocs=*/ 0);
        find("renamed", /*withIndex=*/ false, oldestTS, /*numDocs=*/ 0);
        find("renamed", /*withIndex=*/ true, oldestTS, /*numDocs=*/ 0);
    })();

    (function createTimestampTests() {
        // Reading at 'createTS' from the original collection should not find any document yet.
        // Using the index at this point returns BadValue from hint as it does not exist yet.
        find(coll().getName(), /*withIndex=*/ false, createTS, /*numDocs=*/ 0);
        find(coll().getName(),
             /*withIndex=*/ true,
             createTS,
             /*numDocs=*/ 0,
             /*expectedError=*/ true);

        // Reading at 'createTS' from the renamed collection should not find any document as it was
        // non-existent.
        find("renamed", /*withIndex=*/ false, createTS, /*numDocs=*/ 0);
        find("renamed", /*withIndex=*/ true, createTS, /*numDocs=*/ 0);
    })();

    (function firstInsertTimestampTests() {
        // Reading at 'firstInsertTS' from the original collection should find a document. Using the
        // index at this point returns BadValue from hint as it does not exist yet.
        find(coll().getName(), /*withIndex=*/ false, firstInsertTS, /*numDocs=*/ 1);
        find(coll().getName(),
             /*withIndex=*/ true,
             firstInsertTS,
             /*numDocs=*/ 0,
             /*expectedError=*/ true);

        // Reading at 'firstInsertTS' from the renamed collection should not find any document as it
        // was non-existent.
        find("renamed", /*withIndex=*/ false, firstInsertTS, /*numDocs=*/ 0);
        find("renamed", /*withIndex=*/ true, firstInsertTS, /*numDocs=*/ 0);
    })();

    (function indexTimestampTests() {
        // Reading at 'indexTS' from the original collection should find a document. This includes
        // with the index as it now exists.
        find(coll().getName(), /*withIndex=*/ false, indexTS, /*numDocs=*/ 1);
        find(coll().getName(), /*withIndex=*/ true, indexTS, /*numDocs=*/ 1);

        // Reading at 'indexTS' from the renamed collection should not find any document as it was
        // non-existent.
        find("renamed", /*withIndex=*/ false, indexTS, /*numDocs=*/ 0);
        find("renamed", /*withIndex=*/ true, indexTS, /*numDocs=*/ 0);
    })();

    (function secondInsertTimestampTests() {
        // Reading at 'secondInsertTS' from the original collection should find two documents. This
        // includes with the index as it now exists.
        find(coll().getName(), /*withIndex=*/ false, secondInsertTS, /*numDocs=*/ 2);
        find(coll().getName(), /*withIndex=*/ true, secondInsertTS, /*numDocs=*/ 2);

        // Reading at 'secondInsertTS' from the renamed collection should not find any document as
        // it was non-existent.
        find("renamed", /*withIndex=*/ false, secondInsertTS, /*numDocs=*/ 0);
        find("renamed", /*withIndex=*/ true, secondInsertTS, /*numDocs=*/ 0);
    })();

    (function renameTimestampTests() {
        // Reading at 'renameTS' from the original collection should not find any document as it was
        // non-existent at this time.
        find(coll().getName(), /*withIndex=*/ false, renameTS, /*numDocs=*/ 0);
        find(coll().getName(), /*withIndex=*/ true, renameTS, /*numDocs=*/ 0);

        // Reading at 'renameTS' from the renamed collection should find two documents. This
        // includes with the index as it now exists.
        find("renamed", /*withIndex=*/ false, renameTS, /*numDocs=*/ 2);
        find("renamed", /*withIndex=*/ true, renameTS, /*numDocs=*/ 2);
    })();

    (function dropTimestampTests() {
        // Reading at 'dropTS' from the original collection should not find any document as it was
        // non-existent at this time.
        find(coll().getName(), /*withIndex=*/ false, dropTS, /*numDocs=*/ 0);
        find(coll().getName(), /*withIndex=*/ true, dropTS, /*numDocs=*/ 0);

        // Reading at 'dropTS' from the renamed collection should not find any document as it was
        // non-existent at this time.
        find("renamed", /*withIndex=*/ false, dropTS, /*numDocs=*/ 0);
        find("renamed", /*withIndex=*/ true, dropTS, /*numDocs=*/ 0);
    })();

    (function recreatedTimestampTests() {
        // Reading at 'recreatedTS' from the original collection should not find any document yet.
        // Using the index at this point returns BadValue from hint as it does not exist yet.
        find(coll().getName(), /*withIndex=*/ false, recreatedTS, /*numDocs=*/ 0);
        find(coll().getName(),
             /*withIndex=*/ true,
             recreatedTS,
             /*numDocs=*/ 0,
             /*expectedError=*/ true);

        // Reading at 'recreatedTS' from the renamed collection should not find any document as it
        // was non-existent.
        find("renamed", /*withIndex=*/ false, recreatedTS, /*numDocs=*/ 0);
        find("renamed", /*withIndex=*/ true, recreatedTS, /*numDocs=*/ 0);
    })();

    (function thirdInsertTimestampTests() {
        // Reading at 'thirdInsertTS' from the original collection should find a document. Using the
        // index at this point returns BadValue from hint as it does not exist yet.
        find(coll().getName(), /*withIndex=*/ false, thirdInsertTS, /*numDocs=*/ 1);
        find(coll().getName(),
             /*withIndex=*/ true,
             thirdInsertTS,
             /*numDocs=*/ 0,
             /*expectedError=*/ true);

        // Reading at 'thirdInsertTS' from the renamed collection should not find any document as it
        // was non-existent.
        find("renamed", /*withIndex=*/ false, thirdInsertTS, /*numDocs=*/ 0);
        find("renamed", /*withIndex=*/ true, thirdInsertTS, /*numDocs=*/ 0);
    })();

    (function createAndDropIndexTimestampTests() {
        // Reading at 'createIndexTS' will instantiate a collection and we should be able to use the
        // index that was dropped in the future.
        find(coll().getName(), /*withIndex=*/ true, createIndexTS, /*numDocs=*/ 1);
        find(coll().getName(), /*withIndex=*/ false, createIndexTS, /*numDocs=*/ 1);

        // Reading at 'dropIndexTS' will prevent us from using the index.
        find(coll().getName(),
             /*withIndex=*/ true,
             dropIndexTS,
             /*numDocs=*/ 1,
             /*expectedError=*/ true);
        find(coll().getName(), /*withIndex=*/ false, dropIndexTS, /*numDocs=*/ 1);
    })();
};

runPointInTimeTests();

// Take a checkpoint and restart the node.
assert.commandWorked(db().adminCommand({fsync: 1}));
restart();

// We expect the same results after a restart.
runPointInTimeTests();

replTest.stopSet();
})();
