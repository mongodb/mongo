'use strict';

/**
 * Tests running operations with 'collectionUUID' parameter while the collection is being renamed
 * concurrently, and makes sure that all operations will succeed eventually when using the correct
 * 'collectionUUID' obtained from 'CollectionUUIDMismatch' error.
 * @tags: [
 *   # This test just performs rename operations that can't be executed in transactions.
 *   does_not_support_transactions,
 *   requires_non_retryable_writes,
 *   requires_fcv_60,
 *  ]
 */
load("jstests/libs/fixture_helpers.js");  // For isMongos.
load("jstests/libs/namespace_utils.js");  // For getCollectionNameFromFullNamespace.

const otherDbName = "otherDb";
const otherDbCollName = "otherDbColl";
const sameDbCollName = "sameDbColl";

const adminCommands =
    ["renameCollection", "shardCollection", "reshardCollection", "refineCollectionShardKey"];
const isAdminCommand = function(cmdName) {
    return adminCommands.includes(cmdName);
};

const getUUID = function(database, collName) {
    return database.runCommand({listCollections: 1})
        .cursor.firstBatch.find(c => c.name.startsWith(collName))
        .info.uuid;
};

const executeCommand = function(db, namespace, cmdName, cmdObj) {
    if (isAdminCommand(cmdName)) {
        cmdObj[cmdName] = namespace;
        return db.adminCommand(cmdObj);
    }

    cmdObj[cmdName] = getCollectionNameFromFullNamespace(namespace);
    return db.runCommand(cmdObj);
};

const runCommandInLoop = function(
    db, namespace, cmdName, cmdObj, data, expectedNonRetryableErrors = []) {
    let cmdResult;
    let currentNamespace = namespace;

    // Concurrent renaming might cause these errors.
    const expectedRetryableErrors = [
        ErrorCodes.IllegalOperation,
        ErrorCodes.ConflictingOperationInProgress,
        ErrorCodes.BackgroundOperationInProgressForNamespace,
        ErrorCodes.ReshardCollectionInProgress,
        ErrorCodes.QueryPlanKilled,
        // StaleConfig is usually retried by the mongos, but in situations where multiple errors
        // have ocurred on the same batch and MultipleErrorsOcurred is returned, one of the errors
        // could be StaleConfig and the other could be one that mongos does not retry the batch on.
        ErrorCodes.StaleConfig,
    ];

    let iteration = 0;
    jsTestLog("Thread: " + data.tid + " started to run command: " + cmdName +
              " with expectation to fail with errors: " + tojson(expectedNonRetryableErrors));
    while (true) {
        iteration++;
        cmdResult = executeCommand(db, currentNamespace, cmdName, cmdObj);

        if (cmdResult.writeErrors) {
            cmdResult = cmdResult.writeErrors[0];
        } else if (cmdResult.ok) {
            break;
        }

        if (cmdResult.code === ErrorCodes.MultipleErrorsOccurred) {
            let anyError = cmdResult.errInfo.causedBy[0];
            for (const err of cmdResult.errInfo.causedBy) {
                if (err.code === ErrorCodes.CollectionUUIDMismatch) {
                    anyError = err;
                    break;
                }
            }

            cmdResult = anyError;
        }

        if (expectedRetryableErrors.includes(cmdResult.code)) {
            // Give time for the conflicting in-progress operation to finish.
            sleep(10);
            continue;
        }

        if (cmdResult.code === ErrorCodes.CollectionUUIDMismatch) {
            if (expectedNonRetryableErrors.includes(ErrorCodes.CollectionUUIDMismatch)) {
                break;
            }

            if (cmdResult.actualCollection === null) {
                // Collection uuid shouldn't change unless we are running "reshardCollection".
                assert.eq(data.collUUIDFixed, false);
                data.collUUID = getUUID(db, data.collOriginalName);
                cmdObj["collectionUUID"] = data.collUUID;
            } else {
                currentNamespace = cmdResult.db + "." + cmdResult.actualCollection;
            }

            continue;
        }

        if (expectedNonRetryableErrors.includes(cmdResult.code)) {
            break;
        }

        // Raise exception otherwise.
        throw new Error('Command: ' + tojson(cmdObj) +
                        ' failed with unexpected error: ' + tojson(cmdResult));
    }

    jsTestLog("Thread: " + data.tid + " needed " + iteration +
              " iterations to run command: " + cmdName +
              " with expectation to fail with errors: " + tojson(expectedNonRetryableErrors));
    return cmdResult;
};

const verifyFailingWithCollectionUUIDMismatch = function(
    db, cmdName, cmdObj, collectionUUID, actualCollection, expectedNamespace, data) {
    cmdObj["collectionUUID"] = collectionUUID;
    let res = runCommandInLoop(
        db, expectedNamespace, cmdName, cmdObj, data, [ErrorCodes.CollectionUUIDMismatch]);

    assert.eq(res.db, db.getName());
    assert.eq(res.collectionUUID, collectionUUID);
    assert.eq(res.expectedCollection, getCollectionNameFromFullNamespace(expectedNamespace));
    assert.eq(res.actualCollection, actualCollection);
};

const testCommand = function(
    db, namespace, cmdName, cmdObj, data, expectedNonRetryableErrors = []) {
    verifyFailingWithCollectionUUIDMismatch(
        db, cmdName, cmdObj, data.sameDbCollUUID, sameDbCollName, namespace, data);

    verifyFailingWithCollectionUUIDMismatch(db,
                                            cmdName,
                                            cmdObj,
                                            data.otherDbCollUUID,
                                            // Collection resides on different db won't be returned.
                                            null,
                                            namespace,
                                            data);

    // Verify command eventually succeeds.
    cmdObj["collectionUUID"] = data.collUUID;
    runCommandInLoop(db, namespace, cmdName, cmdObj, data, expectedNonRetryableErrors);
};

var $config = (function() {
    const data = {};

    const states = (function() {
        function init(db, collName) {
            this.Id = 0;
            this.collUUID = getUUID(db, collName);
            this.collUUIDFixed = true;
            this.collOriginalName = collName;
            this.sameDbCollUUID = getUUID(db, sameDbCollName);
            this.otherDbCollUUID = getUUID(db.getSiblingDB(otherDbName), otherDbCollName);
        }

        function rename(db, collName) {
            // Create an unique namespace by appending the thread id and the incremented local id.
            const targetNamespace =
                db.getName() + "." + collName + "_" + this.tid + "_" + this.Id++;
            const srcNamespace = db.getName() + "." + collName;
            const renameCmd = {
                renameCollection: srcNamespace,
                to: targetNamespace,
                dropTarget: false,
                collectionUUID: this.collUUID
            };
            testCommand(db, srcNamespace, "renameCollection", renameCmd, this);
        }

        function crud(db, collName) {
            const id = this.Id++;
            const namespace = db.getName() + "." + collName;

            // Find
            const findCmd = {find: namespace, collectionUUID: this.collUUID};
            testCommand(db, namespace, "find", findCmd, this);

            // Update
            const updateCmd = {
                update: namespace,
                updates: [{q: {x: this.tid}, u: {$set: {updated: id}}, multi: true}],
                collectionUUID: this.collUUID
            };
            testCommand(db, namespace, "update", updateCmd, this);

            // Aggregate
            const aggCmd = {
                aggregate: namespace,
                pipeline: [{$collStats: {latencyStats: {}}}],
                cursor: {},
                collectionUUID: this.collUUID
            };
            testCommand(db, namespace, "aggregate", aggCmd, this);
        }

        function indexCommands(db, collName) {
            const namespace = db.getName() + "." + collName;

            // Create index.
            const indexField = "y_" + this.tid;
            const createIndexCmd = {
                createIndexes: namespace,
                indexes: [{name: indexField, key: {[indexField]: 1}}],
                collectionUUID: this.collUUID
            };
            testCommand(db, namespace, "createIndexes", createIndexCmd, this);

            // CollMod.
            const collModCmd = {
                collMod: namespace,
                index: {keyPattern: {[indexField]: 1}, hidden: true},
                collectionUUID: this.collUUID
            };
            testCommand(db, namespace, "collMod", collModCmd, this);

            // Drop index.
            const dropIndexCmd = {
                dropIndexes: namespace,
                index: {[indexField]: 1},
                collectionUUID: this.collUUID
            };
            // Consecutive drop commands can results in 'IndexNotFound' error, so on retry some
            // shards can fail while others succeed.
            testCommand(
                db, namespace, "dropIndexes", dropIndexCmd, this, [ErrorCodes.IndexNotFound]);
        }

        return {init: init, rename: rename, crud: crud, indexCommands: indexCommands};
    })();

    const setup = function(db, collName, cluster) {
        // Add documents with wide range '_id' to end up with the data distributed across multiple
        // shards in the sharded scenario.
        for (let i = 0; i < this.threadCount; ++i) {
            for (let j = 0; j < 100; ++j) {
                const uniqueNum = i * 100 + j;
                assertAlways.commandWorked(db[collName].insert(
                    {_id: uniqueNum, x: i, ["y_" + i]: uniqueNum, a: uniqueNum, b: uniqueNum}));
            }
        }

        db[sameDbCollName].drop();
        db.getSiblingDB(otherDbName)[otherDbCollName].drop();
        assertAlways.commandWorked(db[sameDbCollName].insert({_id: 0}));
        assertAlways.commandWorked(db.getSiblingDB(otherDbName)[otherDbCollName].insert({_id: 0}));

        if (isMongos(db)) {
            const shardNames = Object.keys(cluster.getSerializedCluster().shards);
            const numShards = shardNames.length;
            if (numShards > 1) {
                assertAlways.commandWorked(
                    db.adminCommand({movePrimary: db.getName(), to: shardNames[0]}));
                assertAlways.commandWorked(db.getSiblingDB(otherDbName).adminCommand({
                    movePrimary: otherDbName,
                    to: shardNames[1]
                }));
            }
        }
    };

    const teardown = function(db, collName, cluster) {
        assert.eq(2, db.getCollectionNames().length);
        assert.eq(1, db.getSiblingDB(otherDbName).getCollectionNames().length);
    };

    const transitions = {
        init: {rename: 1},
        rename: {crud: 1},
        crud: {crud: 0.6, rename: 0.4, indexCommands: 0.2},
        indexCommands: {crud: 0.8, rename: 0.4},
    };

    return {
        threadCount: 10,
        iterations: 64,
        states: states,
        startState: 'init',
        transitions: transitions,
        setup: setup,
        teardown: teardown,
        data: data,
    };
})();
