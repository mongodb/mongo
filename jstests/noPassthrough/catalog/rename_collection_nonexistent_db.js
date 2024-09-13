/**
 * Test the race condition behavior of implicity creating a database through a rename on mongos.
 *
 * @tags: [
 *   assumes_unsharded_collection,
 *   assumes_read_concern_unchanged,
 *   requires_replication,
 *   requires_sharding,
 * ]
 */

import {waitForCurOpByFailPointNoNS} from "jstests/libs/curop_helpers.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

function runTestRenameCollectionOnEvent(st, eventFunction, expectedErrorCode) {
    let testDb = st.s.getDB("sourceDb");
    let portNum = st.s.port;
    const failpointName = "renameWaitAfterDatabaseCreation";
    let sourceCol = testDb.renameDifferentDb;
    sourceCol.drop();

    // Put some documents and indexes in sourceCol.
    assert.commandWorked(sourceCol.insertMany([{a: 1}, {a: 2}, {a: 3}]));
    assert.commandWorked(sourceCol.createIndexes([{a: 1}, {b: 1}]));

    assert.commandWorked(
        testDb.adminCommand({configureFailPoint: failpointName, mode: "alwaysOn"}));

    const renameDone = startParallelShell(funWithArgs(function(expectedErrorCode) {
                                              const targetDB = db.getSiblingDB("sourceDb");

                                              const destDb = db.getSiblingDB("destDb");
                                              let destCol = destDb.renameDifferentDb;
                                              destCol.drop();

                                              assert.commandFailedWithCode(targetDB.adminCommand({
                                                  renameCollection: "sourceDb.renameDifferentDb",
                                                  to: "destDb.renameDifferentDb"
                                              }),
                                                                           expectedErrorCode);
                                          }, expectedErrorCode), portNum);

    waitForCurOpByFailPointNoNS(testDb, failpointName, {}, {localOps: true});

    eventFunction(testDb);

    assert.commandWorked(testDb.adminCommand({configureFailPoint: failpointName, mode: "off"}));
    renameDone();
}

const st = new ShardingTest({shards: 2, mongos: 1, config: 1});

// Tests that the rename command errors if the source database is dropped during execution.
runTestRenameCollectionOnEvent(st, (testDb) => {
    assert.commandWorked(testDb.runCommand({dropDatabase: 1}));
}, ErrorCodes.NamespaceNotFound);

// Tests that the rename command errors if the source collection is dropped during execution.
runTestRenameCollectionOnEvent(st, (testDb) => {
    testDb.renameDifferentDb.drop();
}, ErrorCodes.NamespaceNotFound);

// Tests that the rename command errors if the destination collection is created during execution.
const createDestinationCollection = (testDb) => {
    const destDb = testDb.getSiblingDB("destDb");
    let destCol = destDb.renameDifferentDb;
    assert.commandWorked(destCol.insertMany([{a: 1}, {a: 2}, {a: 3}]));
};
runTestRenameCollectionOnEvent(
    st, createDestinationCollection, [ErrorCodes.NamespaceExists, ErrorCodes.CommandFailed]);

// Tests that the rename command errors if the destination database is dropped right after creation
// during execution.
const dropDestinationDatabase = (testDb) => {
    const destDb = testDb.getSiblingDB("destDb");
    assert.commandWorked(destDb.runCommand({dropDatabase: 1}));
};
runTestRenameCollectionOnEvent(st, dropDestinationDatabase, ErrorCodes.NamespaceNotFound);

st.stop();
