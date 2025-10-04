/**
 * Tests running operations with 'collectionUUID' parameter while the collection is being renamed
 * concurrently, and makes sure that all operations will succeed eventually when using the correct
 * 'collectionUUID' obtained from 'CollectionUUIDMismatch' error.
 * @tags: [
 *   # balancer may move unsplittable collections and change the uuid
 *   assumes_balancer_off,
 *   # This test just performs rename operations that can't be executed in transactions.
 *   does_not_support_transactions,
 *   requires_non_retryable_writes,
 *   requires_fcv_60,
 *  ]
 */
import {isMongos} from "jstests/concurrency/fsm_workload_helpers/server_types.js";
import {getCollectionNameFromFullNamespace} from "jstests/libs/namespace_utils.js";

const otherDbName = "otherDb";
const otherDbCollName = "otherDbColl";
const sameDbCollName = "sameDbColl";

const adminCommands = ["renameCollection", "shardCollection", "reshardCollection", "refineCollectionShardKey"];
const isAdminCommand = function (cmdName) {
    return adminCommands.includes(cmdName);
};

const getUUID = function (database, collName) {
    return database.runCommand({listCollections: 1}).cursor.firstBatch.find((c) => c.name.startsWith(collName)).info
        .uuid;
};

const executeCommand = function (db, namespace, cmdName, cmdObj) {
    if (isAdminCommand(cmdName)) {
        cmdObj[cmdName] = namespace;
        return db.adminCommand(cmdObj);
    }

    cmdObj[cmdName] = getCollectionNameFromFullNamespace(namespace);
    return db.runCommand(cmdObj);
};

const runCommandInLoop = function (db, namespace, cmdName, cmdObj, data, expectedNonRetryableErrors = []) {
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
    jsTestLog(
        "Thread: " +
            data.tid +
            " started to run command: " +
            cmdName +
            " with expectation to fail with errors: " +
            tojson(expectedNonRetryableErrors),
    );
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

        if (
            cmdResult.code === ErrorCodes.OplogOperationUnsupported &&
            cmdResult.errmsg.includes("Command not supported during resharding") &&
            cmdResult.errmsg.includes("commitIndexBuild")
        ) {
            // TODO (SERVER-91708): Resharding should consider queued index builds waiting for
            // active number of index builds to be below the threshold.
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
        throw new Error("Command: " + tojson(cmdObj) + " failed with unexpected error: " + tojson(cmdResult));
    }

    jsTestLog(
        "Thread: " +
            data.tid +
            " needed " +
            iteration +
            " iterations to run command: " +
            cmdName +
            " with expectation to fail with errors: " +
            tojson(expectedNonRetryableErrors),
    );
    return cmdResult;
};

const verifyFailingWithCollectionUUIDMismatch = function (
    db,
    cmdName,
    cmdObj,
    collectionUUID,
    actualCollection,
    expectedNamespace,
    data,
) {
    cmdObj["collectionUUID"] = collectionUUID;
    let res = runCommandInLoop(db, expectedNamespace, cmdName, cmdObj, data, [ErrorCodes.CollectionUUIDMismatch]);

    assert.eq(res.db, db.getName());
    assert.eq(res.collectionUUID, collectionUUID);
    assert.eq(res.expectedCollection, getCollectionNameFromFullNamespace(expectedNamespace));
    assert.eq(res.actualCollection, actualCollection);
};

export const testCommand = function (db, namespace, cmdName, cmdObj, data, expectedNonRetryableErrors = []) {
    verifyFailingWithCollectionUUIDMismatch(db, cmdName, cmdObj, data.sameDbCollUUID, sameDbCollName, namespace, data);

    verifyFailingWithCollectionUUIDMismatch(
        db,
        cmdName,
        cmdObj,
        data.otherDbCollUUID,
        // Collection resides on different db won't be returned.
        null,
        namespace,
        data,
    );

    // Verify command eventually succeeds.
    cmdObj["collectionUUID"] = data.collUUID;
    runCommandInLoop(db, namespace, cmdName, cmdObj, data, expectedNonRetryableErrors);
};

export const $config = (function () {
    const data = {};

    const states = (function () {
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
            const targetNamespace = db.getName() + "." + collName + "_" + this.tid + "_" + this.Id++;
            const srcNamespace = db.getName() + "." + collName;
            const renameCmd = {
                renameCollection: srcNamespace,
                to: targetNamespace,
                dropTarget: false,
                collectionUUID: this.collUUID,
            };
            testCommand(db, srcNamespace, "renameCollection", renameCmd, this);
        }

        function crud(db, collName) {
            const id = this.Id++;
            const namespace = db.getName() + "." + collName;

            // Find
            // Use 'singleBatch: true' to avoid leaving open cursors.
            const findCmd = {find: namespace, collectionUUID: this.collUUID, singleBatch: true};
            testCommand(db, namespace, "find", findCmd, this);

            // Update
            const updateCmd = {
                update: namespace,
                updates: [{q: {x: this.tid}, u: {$set: {updated: id}}, multi: true}],
                collectionUUID: this.collUUID,
            };
            testCommand(db, namespace, "update", updateCmd, this);

            // Aggregate
            const aggCmd = {
                aggregate: namespace,
                pipeline: [{$collStats: {latencyStats: {}}}],
                cursor: {},
                collectionUUID: this.collUUID,
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
                collectionUUID: this.collUUID,
            };
            testCommand(db, namespace, "createIndexes", createIndexCmd, this);

            // CollMod.
            const collModCmd = {
                collMod: namespace,
                index: {keyPattern: {[indexField]: 1}, hidden: true},
                collectionUUID: this.collUUID,
            };
            testCommand(db, namespace, "collMod", collModCmd, this);

            // Drop index.
            const dropIndexCmd = {
                dropIndexes: namespace,
                index: {[indexField]: 1},
                collectionUUID: this.collUUID,
            };
            // Consecutive drop commands can results in 'IndexNotFound' error, so on retry some
            // shards can fail while others succeed.
            testCommand(db, namespace, "dropIndexes", dropIndexCmd, this, [ErrorCodes.IndexNotFound]);
        }

        return {init: init, rename: rename, crud: crud, indexCommands: indexCommands};
    })();

    const setup = function (db, collName, cluster) {
        if (isMongos(db)) {
            db.getSiblingDB(otherDbName).dropDatabase();
            db[sameDbCollName].drop();
            const shardNames = Object.keys(cluster.getSerializedCluster().shards);
            const numShards = shardNames.length;
            let otherDbShard;

            if (numShards > 1) {
                const currDb = db.getSiblingDB("config")["databases"].findOne({_id: db.getName()});
                shardNames.some((shard) => {
                    if (shard != currDb.primary) {
                        otherDbShard = shard;
                        return false;
                    }
                    return true;
                });
            } else {
                otherDbShard = shardNames[0];
            }
            assert.commandWorked(db.adminCommand({enableSharding: otherDbName, primaryShard: otherDbShard}));
        } else {
            db[sameDbCollName].drop();
            db.getSiblingDB(otherDbName)[otherDbCollName].drop();
        }

        // Add documents with wide range '_id' to end up with the data distributed across multiple
        // shards in the sharded scenario.
        for (let i = 0; i < this.threadCount; ++i) {
            for (let j = 0; j < 100; ++j) {
                const uniqueNum = i * 100 + j;
                assert.commandWorked(
                    db[collName].insert({_id: uniqueNum, x: i, ["y_" + i]: uniqueNum, a: uniqueNum, b: uniqueNum}),
                );
            }
        }
        assert.commandWorked(db[sameDbCollName].insert({_id: 0}));
        assert.commandWorked(db.getSiblingDB(otherDbName)[otherDbCollName].insert({_id: 0}));
    };

    const teardown = function (db, collName, cluster) {
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
        startState: "init",
        transitions: transitions,
        setup: setup,
        teardown: teardown,
        data: data,
    };
})();
