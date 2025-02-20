// Test that open transactions block DDL operations on the involved collections.
//
// @tags: [
//   # The test runs commands that are not allowed with security token: endSession.
//   not_allowed_with_signed_security_token,
//   uses_rename,
//   uses_transactions,
//   # This test relies on mapOnEachShardNode, which forces a new connection to all
//   # nodes. This function can race with initial sync and fail to open the conn.
//   incompatible_with_initial_sync
// ]

import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {Thread} from "jstests/libs/parallelTester.js";

const dbName = "transactions_block_ddl";
const collName = "transactions_block_ddl";
const otherDBName = "transactions_block_ddl_other";
const otherCollName = "transactions_block_ddl_other";
const testDB = db.getSiblingDB(dbName);

const session = testDB.getMongo().startSession({causalConsistency: false});
const sessionDB = session.getDatabase(dbName);
const sessionColl = sessionDB[collName];

/**
 * Drops the test collection, recreates the collection and index before running each test.
 */
function runSetup() {
    sessionDB.runCommand({drop: collName, writeConcern: {w: "majority"}});
    assert.commandWorked(sessionColl.runCommand({
        createIndexes: collName,
        indexes: [{
            key: {
                "b": 1,
            },
            name: "b_1"
        }],
        writeConcern: {w: "majority"}
    }));
}

/**
 * Tests that DDL operations block on transactions and fail when their maxTimeMS expires.
 */
function testTimeout(cmdDBName, ddlCmd) {
    // Setup.
    runSetup();

    session.startTransaction();
    assert.commandWorked(sessionColl.insert({a: 5, b: 6}));
    assert.commandFailedWithCode(
        testDB.getSiblingDB(cmdDBName).runCommand(Object.assign({}, ddlCmd, {maxTimeMS: 500})),
        ErrorCodes.MaxTimeMSExpired);
    assert.commandWorked(session.commitTransaction_forTesting());
}

/**
 * Tests that DDL operations block on transactions but can succeed once the transaction commits.
 */
function testSuccessOnTxnCommit(cmdDBName, ddlCmd, currentOpFilter) {
    // Setup.
    runSetup();

    jsTestLog("About to start tranasction");
    session.startTransaction();
    assert.commandWorked(sessionColl.insert({a: 5, b: 6}));
    jsTestLog("Transaction started, running ddl operation " + ddlCmd);
    let thread = new Thread(function(cmdDBName, ddlCmd) {
        return db.getSiblingDB(cmdDBName).runCommand(ddlCmd);
    }, cmdDBName, ddlCmd);
    thread.start();
    // Wait for the DDL operation to have pending locks.
    assert.soon(
        function() {
            // Note that we cannot use the $currentOp agg stage because it acquires locks
            // (SERVER-35289).
            let currOpResult = FixtureHelpers.mapOnEachShardNode({
                db: testDB.getSiblingDB("admin"),
                func: (testDB) => testDB.currentOp({$and: [currentOpFilter]}).inprog.length === 1,
                primaryNodeOnly: true,
            });

            return currOpResult.includes(true);
        },
        function() {
            let currOpResult = FixtureHelpers.mapOnEachShardNode({
                db: testDB.getSiblingDB("admin"),
                func: (testDB) => testDB.currentOp().inprog,
                primaryNodeOnly: true,
            });

            return "Failed to find DDL command in currentOp output: " + tojson(currOpResult);
        });
    jsTestLog("Committing transaction");
    assert.commandWorked(session.commitTransaction_forTesting());
    jsTestLog("Transaction committed, waiting for ddl operation to complete.");
    thread.join();
    assert.commandWorked(thread.returnData());
}

jsTestLog("Testing that 'drop' blocks on transactions");
const dropCmd = {
    drop: collName,
    writeConcern: {w: "majority"}
};
testTimeout(dbName, dropCmd);
testSuccessOnTxnCommit(dbName, dropCmd, {
    $or: [
        {"command._shardsvrParticipantBlock": collName},
        {$and: [{"command.drop": collName}, {waitingForLock: true}]}
    ]
});

jsTestLog("Testing that 'dropDatabase' blocks on transactions");
const dropDatabaseCmd = {
    dropDatabase: 1,
    writeConcern: {w: "majority"}
};
testTimeout(dbName, dropDatabaseCmd);
testSuccessOnTxnCommit(dbName, dropDatabaseCmd, {
    $or: [
        {"command._shardsvrDropDatabase": 1},
        {$and: [{"command.dropDatabase": 1}, {waitingForLock: true}]}
    ]
});

{
    jsTestLog("Testing that 'renameCollection' within databases blocks on transactions");
    function undoTimedOutRenameIfNeeded(originalFrom, originalTo) {
        // In sharded clusters, the deadline expiration of a DDL command causes the user request to
        // be aborted, but the event won't be propagated to the shard that is currently processing
        // it: as a matter of fact, the execution of the user request may be resumed and completed
        // once the conflicting transaction gets committed.
        // This behavior can cause unexpected failures when running subsequent commands: to remove
        // them, we restore the initial state through a specular request.
        if (FixtureHelpers.isMongos(db) || TestData.testingReplicaSetEndpoint) {
            assert.commandWorkedOrFailedWithCode(testDB.adminCommand({
                renameCollection: originalTo,
                to: originalFrom,
                writeConcern: {w: "majority"}
            }),
                                                 [ErrorCodes.NamespaceNotFound]);
        }
    }
    assert.commandWorked(testDB.runCommand({drop: otherCollName, writeConcern: {w: "majority"}}));
    const renameCollectionCmdSameDB = {
        renameCollection: sessionColl.getFullName(),
        to: dbName + "." + otherCollName,
        writeConcern: {w: "majority"}
    };
    testTimeout("admin", renameCollectionCmdSameDB);
    undoTimedOutRenameIfNeeded(renameCollectionCmdSameDB.renameCollection,
                               renameCollectionCmdSameDB.to);
    testSuccessOnTxnCommit("admin", renameCollectionCmdSameDB, {
        $or: [
            {"command._shardsvrRenameCollectionParticipant": collName},
            {
                $and: [
                    {"command.renameCollection": sessionColl.getFullName()},
                    {waitingForLock: true}
                ]
            }
        ]
    });

    jsTestLog("Testing that 'renameCollection' across databases blocks on transactions");
    if (FixtureHelpers.isMongos(db)) {
        // Ensure that the two databases are assigned to the same primary shard to ensure that
        // renameCollection will succeed.
        assert.commandWorked(testDB.getSiblingDB(otherDBName).dropDatabase());
        assert.commandWorked(db.adminCommand({
            enableSharding: sessionDB.getName(),
            primaryShard: sessionDB.getDatabasePrimaryShardId()
        }));
    } else {
        assert.commandWorked(testDB.getSiblingDB(otherDBName)
                                 .runCommand({drop: otherCollName, writeConcern: {w: "majority"}}));
    }

    const renameCollectionCmdDifferentDB = {
        renameCollection: sessionColl.getFullName(),
        to: otherDBName + "." + otherCollName,
        writeConcern: {w: "majority"}
    };
    testTimeout("admin", renameCollectionCmdDifferentDB);
    undoTimedOutRenameIfNeeded(renameCollectionCmdDifferentDB.renameCollection,
                               renameCollectionCmdDifferentDB.to);
    testSuccessOnTxnCommit("admin", renameCollectionCmdDifferentDB, {
        $or: [
            {"command._shardsvrRenameCollectionParticipant": collName},
            {
                $and: [
                    {"command.renameCollection": sessionColl.getFullName()},
                    {waitingForLock: true}
                ]
            }
        ]
    });
}

jsTestLog("Testing that 'createIndexes' blocks on transactions");
// The transaction will insert a document that has a field 'a'.
const createIndexesCmd = {
    createIndexes: collName,
    indexes: [{key: {a: 1}, name: "a_1"}],
    writeConcern: {w: "majority"}
};
testTimeout(dbName, createIndexesCmd);
testSuccessOnTxnCommit(dbName,
                       createIndexesCmd,
                       {$and: [{"command.createIndexes": collName}, {waitingForLock: true}]});

jsTestLog("Testing that 'dropIndexes' blocks on transactions");
// The setup creates an index on {b: 1} called 'b_1'. The transaction will insert a document
// that has a field 'b'.
const dropIndexesCmd = {
    dropIndexes: collName,
    index: "b_1",
    writeConcern: {w: "majority"}
};
testTimeout(dbName, dropIndexesCmd);
testSuccessOnTxnCommit(
    dbName, dropIndexesCmd, {$and: [{"command.dropIndexes": collName}, {waitingForLock: true}]});
session.endSession();
