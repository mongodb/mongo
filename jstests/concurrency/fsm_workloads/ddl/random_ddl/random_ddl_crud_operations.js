/**
 * Performs a series of CRUD operations while DDL commands are running in the background
 * and verifies guarantees are not broken.
 *
 * When run with the balancer enabled, this test also covers concurrent chunk operations.
 *
 * @tags: [
 *   requires_sharding,
 *   does_not_support_causal_consistency,
 *   # The mutex mechanism used in CRUD and drop states does not support stepdown
 *   does_not_support_stepdowns,
 *   # Relies on internalInsertMaxBatchSize to be 64 or above, but it may be fuzzed to lower values.
 *   does_not_support_config_fuzzer,
 *  ]
 */

import {fsm} from "jstests/concurrency/fsm_libs/fsm.js";
import {uniformDistTransitions} from "jstests/concurrency/fsm_workload_helpers/state_transition_utils.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {ShardingTopologyHelpers} from "jstests/concurrency/fsm_workload_helpers/catalog_and_routing/sharding_topology_helpers.js";

export const $config = (function () {
    function countDocuments(coll, query) {
        let count;
        assert.soon(() => {
            try {
                count = coll.countDocuments(query);
                return true;
            } catch (e) {
                if (e.code === ErrorCodes.QueryPlanKilled) {
                    // Retry. Can happen due to concurrent rename collection.
                    return false;
                }
                throw e;
            }
        });

        return count;
    }

    // Keep data less then 64 (internalInsertMaxBatchSize) to avoid insertMany to yield while
    // inserting. This might cause an rename to execute during the insertMany and post-assertions
    // checks to fail.
    let data = {
        originalImplicitRetryDdlOnConflictWithMigration: null,
        numChunks: 20,
        documentsPerChunk: 3,
        CRUDMutex: "CRUDMutex",
        CRUDMutexDb: `${jsTest.name()}_Mutex`,
        kReshardingAcceptableErrors: [
            // Concurrent resharding with the same collection is ongoing
            ErrorCodes.ConflictingOperationInProgress,
            ErrorCodes.ReshardCollectionInProgress,
            // The collection got dropped so there's nothing to reshard
            ErrorCodes.NamespaceNotSharded,
            ErrorCodes.NamespaceNotFound,
        ],
        threadCollectionName: function (prefix, tid) {
            return prefix + tid;
        },
        // Transitions between dedicated and embedded config shards can sometimes yield a ConflictingOperationInProgress during checkMetadataConsistency. Ignore errors in those cases as it's a transient issue.
        kCheckMetadataAcceptableErrors: TestData.shardsAddedRemoved
            ? [ErrorCodes.ConflictingOperationInProgress]
            : [],

        assertWriteWorked: function (
            cmd,
            retryableErrorCodes,
            ignorableErrorCodes = [],
            retryableErrorCodesInTransaction = [],
        ) {
            if (!Array.isArray(retryableErrorCodes)) {
                retryableErrorCodes = [retryableErrorCodes];
            }
            if (!Array.isArray(ignorableErrorCodes)) {
                ignorableErrorCodes = [ignorableErrorCodes];
            }

            let res = undefined;
            assert.soon(() => {
                try {
                    res = cmd();
                    assert.commandWorked(res);
                    return true;
                } catch (err) {
                    // Bulk write executions throw an exception unlike all the other writes.
                    if (err instanceof BulkWriteError && err.hasWriteErrors()) {
                        const writeErrors = err.getWriteErrors();
                        if (
                            writeErrors.some(
                                (writeErr) =>
                                    !retryableErrorCodes.includes(writeErr.code) &&
                                    !ignorableErrorCodes.includes(writeErr.code),
                            )
                        ) {
                            throw err;
                        }
                        const retryableWriteErrors = writeErrors.filter((writeErr) =>
                            retryableErrorCodes.includes(writeErr.code),
                        );
                        if (retryableWriteErrors.length > 0) {
                            if (
                                fsm.isInvocationRunningInsideTransaction(this) &&
                                retryableWriteErrors.some(
                                    (writeErr) =>
                                        !retryableErrorCodesInTransaction.includes(writeErr.code),
                                )
                            ) {
                                fsm.forceRunningOutsideTransaction(this);
                            }
                            res = undefined;
                            return false;
                        }
                        return true;
                    }
                    // We expect that the errors that follow are the result of simple write errors. Any other error
                    // cannot be treated and must be bubbled upwards. An example of this could be simple transaction
                    // failures.
                    if (res === undefined) {
                        throw err;
                    }
                    if (!(res instanceof WriteResult)) {
                        throw res;
                    }
                    const writeErr = res.getWriteError();
                    if (retryableErrorCodes.includes(writeErr.code)) {
                        if (
                            fsm.isInvocationRunningInsideTransaction(this) &&
                            !retryableErrorCodesInTransaction.includes(writeErr.code)
                        ) {
                            // TODO SERVER-132267: movePrimary has poor interactions with transactions and can
                            // deadlock during testing due to transaction not having a timeout set. In this case
                            // we cause the operation to fail and the entire state to be retried outside of the
                            // transaction. This will unblock movePrimary and let it finish.
                            fsm.forceRunningOutsideTransaction(this);
                        }
                        res = undefined;
                        return false;
                    }
                    if (ignorableErrorCodes.includes(writeErr.code)) {
                        return true;
                    }
                    // At this point the error received seems to be a real issue, bubble it upwards
                    throw err;
                }
            });
            return res;
        },
        /**
         * Used for mutual exclusion. Uses a collection to ensure atomicity on the read and update
         * operation. Uses a different session to avoid having issues with multi-document transaction
         * snapshots.
         */
        mutexLock: function (mutexSession, tid, collName) {
            jsTest.log.info("Trying to acquire mutexLock for resource", {
                tid,
                collection: collName,
            });
            const sessionDb = mutexSession.getDatabase(this.CRUDMutexDb);
            try {
                assert.soon(() => {
                    const res = this.assertWriteWorked(() =>
                        sessionDb[data.CRUDMutex].update({tid: tid, mutex: 0}, {$set: {mutex: 1}}),
                    );
                    return res.nModified === 1;
                });
            } catch (e) {
                jsTest.log.info("Failed to acquire lock", {tid, collection: collName, e});
                throw e;
            }
            jsTest.log.info("Acquired mutexLock", {tid, collection: collName});
        },
        mutexUnlock: function (mutexSession, tid, collName) {
            const sessionDb = mutexSession.getDatabase(this.CRUDMutexDb);
            try {
                this.assertWriteWorked(() =>
                    sessionDb[data.CRUDMutex].update({tid: tid}, {$set: {mutex: 0}}),
                );
            } catch (e) {
                jsTest.log.info("Failed to unlock lock", {tid, collection: collName, e});
                throw e;
            }
            jsTest.log.info("Unlocked lock", {tid, collection: collName});
        },
    };

    let states = {
        init: function (db, collName, connCache) {
            this.collName = this.threadCollectionName(collName, this.tid);
            this.mutexSession = db.getMongo().startSession();
        },

        createUnsplittable: function (db, collName, connCache) {
            let tid = this.tid;
            // Pick a tid at random until we pick one that doesn't target this thread's collection.
            while (tid === this.tid) tid = Random.randInt(this.threadCount);

            const targetThreadColl = this.threadCollectionName(collName, tid);
            jsTest.log.info("createUnsplittable state", {
                tid,
                currentTid: this.tid,
                collection: targetThreadColl,
            });
            try {
                const res = assert.commandWorkedOrFailedWithCode(
                    db.runCommand({
                        createUnsplittableCollection: targetThreadColl,
                    }),
                    [
                        ErrorCodes.ConflictingOperationInProgress,
                        ErrorCodes.AlreadyInitialized,
                        ErrorCodes.InvalidOptions,
                        ErrorCodes.NamespaceExists,
                        ErrorCodes.CannotCreateCollection,
                        ErrorCodes.MovePrimaryInProgress,
                    ],
                );
                if (!res.ok) {
                    // If we encounter a failure and we're inside a multi-document transaction then the state succeeding
                    // will trigger the commit to fail since the transaction will get aborted. This in turn would cause
                    // the transaction to be retried indefinitely. To prevent this we force the state to be retried outside
                    // of the transaction in order to let it make forward progress.
                    fsm.forceRunningOutsideTransaction(this);
                }
            } finally {
                jsTest.log.info("createUnsplittable finished", {
                    tid,
                    currentTid: this.tid,
                    collection: targetThreadColl,
                });
            }
        },
        create: function (db, collName, connCache) {
            let tid = this.tid;
            // Pick a tid at random until we pick one that doesn't target this thread's collection.
            while (tid === this.tid) tid = Random.randInt(this.threadCount);

            const targetThreadColl = this.threadCollectionName(collName, tid);
            const coll = db[targetThreadColl];
            const fullNs = coll.getFullName();
            jsTest.log.info("create state", {
                tid,
                currentTid: this.tid,
                collection: targetThreadColl,
            });
            try {
                assert.commandWorked(
                    db.adminCommand({
                        shardCollection: fullNs,
                        key: {[`tid_${tid}_0`]: 1},
                        unique: false,
                    }),
                );
            } catch (e) {
                const exceptionCode = e.code;
                if (exceptionCode) {
                    if (
                        exceptionCode == ErrorCodes.ConflictingOperationInProgress ||
                        exceptionCode == ErrorCodes.AlreadyInitialized ||
                        exceptionCode == ErrorCodes.InvalidOptions
                    ) {
                        // It is fine for a shardCollection to throw AlreadyInitialized, a
                        // resharding state might have changed the shard key for the namespace. It
                        // is also fine to fail with InvalidOptions, a drop state could've removed
                        // the indexes and the CRUD state might have added some documents, forcing
                        // the need to manually create indexes.
                        return;
                    }
                }
                throw e;
            } finally {
                jsTest.log.info("create state finished", {
                    tid,
                    currentTid: this.tid,
                    collection: targetThreadColl,
                });
            }
        },
        drop: function (db, collName, connCache) {
            let tid = this.tid;
            // Pick a tid at random until we pick one that doesn't target this thread's collection.
            while (tid === this.tid) tid = Random.randInt(this.threadCount);

            const targetThreadColl = this.threadCollectionName(collName, tid);

            jsTest.log.info("drop state", {
                tid,
                currentTid: this.tid,
                collection: targetThreadColl,
            });
            this.mutexLock(this.mutexSession, tid, targetThreadColl);
            try {
                assert.eq(db[targetThreadColl].drop(), true);
            } finally {
                this.mutexUnlock(this.mutexSession, tid, targetThreadColl);
                jsTest.log.info("drop state finished", {
                    tid,
                    currentTid: this.tid,
                    collection: targetThreadColl,
                });
            }
        },
        rename: function (db, collName, connCache) {
            let tid = this.tid;
            // Pick a tid at random until we pick one that doesn't target this thread's collection.
            while (tid === this.tid) tid = Random.randInt(this.threadCount);
            const srcCollName = this.threadCollectionName(collName, tid);
            const srcColl = db[srcCollName];
            // Rename collection
            const destCollName = this.threadCollectionName(
                collName,
                tid + "_" + extractUUIDFromObject(UUID()),
            );
            try {
                jsTest.log.info("rename state", {
                    tid,
                    currentTid: this.tid,
                    collection: srcCollName,
                    dst: destCollName,
                });
                assert.commandWorked(srcColl.renameCollection(destCollName));
            } catch (e) {
                const exceptionCode = e.code;
                if (exceptionCode) {
                    if (exceptionCode === ErrorCodes.NamespaceNotFound) {
                        // It is fine for a rename operation to throw NamespaceNotFound BEFORE
                        // starting (e.g. if the collection was previously dropped). Checking the
                        // changelog to assert that no such exception was thrown AFTER a rename
                        // started.
                        const dbName = db.getName();
                        let config = db.getSiblingDB("config");
                        let countRenames = config.changelog
                            .find({
                                what: "renameCollection.start",
                                details: {
                                    source: dbName + srcCollName,
                                    destination: dbName + destCollName,
                                },
                            })
                            .itcount();
                        assert.eq(
                            0,
                            countRenames,
                            "NamespaceNotFound exception thrown during rename from " +
                                srcCollName +
                                " to " +
                                destCollName,
                        );
                        return;
                    }
                    if (exceptionCode === ErrorCodes.ConflictingOperationInProgress) {
                        // It is fine for a rename operation to throw ConflictingOperationInProgress
                        // if a concurrent rename with the same source collection but different
                        // options is ongoing.
                        return;
                    }
                }
                throw e;
            } finally {
                jsTest.log.info("rename state finished", {
                    tid,
                    currentTid: this.tid,
                    collection: srcCollName,
                    dst: destCollName,
                });
            }
        },
        resharding: function (db, collName, connCache) {
            let tid = this.tid;
            // Pick a tid at random until we pick one that doesn't target this thread's collection.
            while (tid === this.tid) tid = Random.randInt(this.threadCount);
            const fullNs = db[this.threadCollectionName(collName, tid)].getFullName();
            let newKey = "tid_" + tid + "_" + Random.randInt(2);
            const isHashed = Random.rand() < 0.5;

            jsTest.log.info("resharding state", {
                tid,
                currentTid: this.tid,
                collection: fullNs,
                newKey,
                isHashed,
            });

            // We explicitly flip between hashed/plain shard keys because hashed keys can be pre-split.
            // This is important for suites that have balancing enabled because those will trigger chunk
            // operations in the background of this test which is a desirable testing action here.
            assert.commandWorkedOrFailedWithCode(
                db.adminCommand({
                    reshardCollection: fullNs,
                    key: {[`${newKey}`]: isHashed ? "hashed" : 1},
                    numInitialChunks: isHashed ? 10 : 1,
                }),
                this.kReshardingAcceptableErrors,
            );

            jsTest.log.info("resharding state finished", {
                tid,
                currentTid: this.tid,
                collection: fullNs,
                newKey,
                isHashed,
            });
        },
        checkDatabaseMetadataConsistency: function (db, collName, connCache) {
            jsTest.log.info("Check database metadata state", {tid: this.tid});
            try {
                const inconsistencies = db.checkMetadataConsistency().toArray();
                assert.eq(0, inconsistencies.length, tojson(inconsistencies));
            } catch (e) {
                if (!this.kCheckMetadataAcceptableErrors.includes(e.code)) {
                    throw e;
                }
            } finally {
                jsTest.log.info("Check database metadata state finished", {tid: this.tid});
            }
        },
        checkCollectionMetadataConsistency: function (db, collName, connCache) {
            let tid = this.tid;
            while (tid === this.tid) tid = Random.randInt(this.threadCount);

            const targetThreadColl = this.threadCollectionName(collName, tid);
            jsTest.log.info("Check collection metadata state", {
                tid,
                currentTid: this.tid,
                collection: targetThreadColl,
            });
            try {
                const inconsistencies = db[targetThreadColl].checkMetadataConsistency().toArray();
                assert.eq(0, inconsistencies.length, tojson(inconsistencies));
            } catch (e) {
                if (!this.kCheckMetadataAcceptableErrors.includes(e.code)) {
                    throw e;
                }
            } finally {
                jsTest.log.info("Check collection metadata state finished", {
                    tid,
                    currentTid: this.tid,
                    collection: targetThreadColl,
                });
            }
        },
        unshardCollection: function unshardCollection(db, collName, connCache) {
            let tid = this.tid;
            while (tid === this.tid) tid = Random.randInt(this.threadCount);

            const targetThreadColl = this.threadCollectionName(collName, tid);
            const namespace = `${db}.${targetThreadColl}`;
            jsTest.log.info("Started to unshard collection", {tid, namespace});
            try {
                assert.commandWorkedOrFailedWithCode(
                    db.adminCommand({unshardCollection: namespace}),
                    this.kReshardingAcceptableErrors,
                );
            } finally {
                jsTest.log.info("Unsharding completed", {tid, namespace});
            }
        },
        untrackUnshardedCollection: function untrackUnshardedCollection(db, collName, connCache) {
            let tid = this.tid;
            while (tid === this.tid) tid = Random.randInt(this.threadCount);

            const targetThreadColl = this.threadCollectionName(collName, tid);
            const namespace = `${db}.${targetThreadColl}`;
            try {
                jsTest.log.info("Started to untrack collection", {tid, namespace});
                // Attempt to unshard the collection first
                jsTest.log.info("1. Attempting to unshard collection", {tid, namespace});
                const res = assert.commandWorkedOrFailedWithCode(
                    db.adminCommand({unshardCollection: namespace}),
                    this.kReshardingAcceptableErrors,
                );
                jsTest.log.info(`Unsharding completed`, {tid, namespace});
                jsTest.log.info(`2. Untracking collection`, {tid, namespace});
                // Note this command will behave as no-op in case the collection is not tracked.
                assert.commandWorkedOrFailedWithCode(
                    db.adminCommand({untrackUnshardedCollection: namespace}),
                    [
                        // Handles the case where the collection is not located on its primary
                        ErrorCodes.OperationFailed,
                        // Handles the case where the collection is sharded
                        ErrorCodes.InvalidNamespace,
                        // Handles the case where the collection/db does not exist
                        ErrorCodes.NamespaceNotFound,
                    ],
                );
            } finally {
                jsTest.log.info(`Untrack collection completed`, {tid, namespace});
            }
        },
        movePrimary: function (db, collName, connCache) {
            // Move the primary shard of the database to a random shard (which could coincide with
            // the starting one).
            const shards = ShardingTopologyHelpers.getShardNames(db);
            const toShard = shards[Random.randInt(shards.length)];
            jsTest.log.info("Running movePrimary", {tid: this.tid, db, toShard});

            const expectedErrorCodes = [
                // Caused by a concurrent movePrimary operation on the same database but a
                // different destination shard.
                ErrorCodes.ConflictingOperationInProgress,
                // Due to a stepdown of the donor during the cloning phase, the movePrimary
                // operation failed. It is not automatically recovered, but any orphaned data on
                // the recipient has been deleted.
                7120202,
                // In the FSM tests, there is a chance that there are still some User
                // collections left to clone. This occurs when a MovePrimary joins an already
                // existing MovePrimary command that has purposefully triggered a failpoint.
                9046501,
            ];
            if (TestData.hasRandomShardsAddedRemoved) {
                expectedErrorCodes.push(ErrorCodes.ShardNotFound);
            }
            try {
                assert.commandWorkedOrFailedWithCode(
                    db.adminCommand({movePrimary: db.getName(), to: toShard}),
                    expectedErrorCodes,
                );
            } finally {
                jsTest.log.info("Finished movePrimary", {tid: this.tid, db, toShard});
            }
        },
        CRUD: function (db, collName, connCache) {
            let tid = this.tid;
            // Pick a tid at random until we pick one that doesn't target this thread's collection.
            while (tid === this.tid) tid = Random.randInt(this.threadCount);

            const targetThreadColl = this.threadCollectionName(collName, tid);
            jsTest.log.info("CRUD state", {
                tid,
                currentTid: this.tid,
                collection: targetThreadColl,
            });

            this.mutexLock(this.mutexSession, tid, targetThreadColl);

            try {
                const coll = db[targetThreadColl];

                const generation = new Date().getTime();
                // Insert Data
                const numDocs = data.documentsPerChunk * data.numChunks;

                jsTest.log.info("CRUD - Insert", {
                    tid,
                    currentTid: this.tid,
                    collection: targetThreadColl,
                });
                this.assertWriteWorked(
                    () => {
                        let insertBulkOp = coll.initializeUnorderedBulkOp();
                        for (let i = 0; i < numDocs; ++i) {
                            insertBulkOp.insert({
                                _id: `${generation}_${i}`,
                                generation: generation,
                                count: i,
                                [`tid_${tid}_0`]: i,
                                [`tid_${tid}_1`]: i,
                            });
                        }
                        return insertBulkOp.execute();
                    },
                    ErrorCodes.MovePrimaryInProgress,
                    // TODO (SERVER-32113): Retryable writes may cause double inserts if performed on a
                    // shard involved as the originator of a movePrimary operation.
                    ErrorCodes.DuplicateKey,
                );

                let currentDocs = countDocuments(coll, {generation: generation});

                // Check guarantees IF NO CONCURRENT DROP is running.
                // If a concurrent rename came in, then either the full operation succeded (meaning
                // there will be 0 documents left) or the insert came in first.
                assert.contains(currentDocs, [0, numDocs], {
                    tid,
                    currentTid: this.tid,
                    collection: targetThreadColl,
                });

                jsTest.log.info("CRUD - Update", {
                    tid,
                    currentTid: this.tid,
                    collection: targetThreadColl,
                });
                const res = this.assertWriteWorked(
                    () =>
                        coll.update(
                            {generation: generation},
                            {$set: {updated: true}},
                            {multi: true},
                        ),
                    ErrorCodes.MovePrimaryInProgress,
                    ErrorCodes.QueryPlanKilled,
                );
                if (res instanceof WriteResult && res.hasWriteError()) {
                    // Update is expected to throw ErrorCodes::QueryPlanKilled if performed
                    // concurrently with a rename (SERVER-31695).
                    assert.writeErrorWithCode(res, ErrorCodes.QueryPlanKilled);
                    jsTest.log.info("CRUD state finished earlier because query plan was killed", {
                        tid,
                        currentTid: this.tid,
                        collection: targetThreadColl,
                    });
                    return;
                }

                // Delete Data
                jsTest.log.info("CRUD - Remove", {
                    tid,
                    currentTid: this.tid,
                    collection: targetThreadColl,
                });
                // Check if delete succeeded
                this.assertWriteWorked(
                    () => coll.remove({generation: generation}, {multi: true}),
                    ErrorCodes.MovePrimaryInProgress,
                );
                // Check guarantees IF NO CONCURRENT DROP is running.
                assert.eq(countDocuments(coll, {generation: generation}), 0, {
                    tid,
                    currentTid: this.tid,
                    collection: targetThreadColl,
                });
            } finally {
                this.mutexUnlock(this.mutexSession, tid, targetThreadColl);
                jsTest.log.info("CRUD state finished", {
                    tid,
                    currentTid: this.tid,
                    collection: targetThreadColl,
                });
            }
        },
    };

    let setup = function (db, collName, cluster) {
        // Balancer-based suites inject background hook to automatically handle errors caused by multiple incompatible DDL operations on each request.
        // Such a behavior may cause this workload to starve, so it gets disabled.
        this.originalImplicitRetryDdlOnConflictWithMigration =
            TestData.implicitRetryDdlOnConflictWithMigration;
        TestData.implicitRetryDdlOnConflictWithMigration = false;
        const mutexDb = db.getSiblingDB(data.CRUDMutexDb);
        for (let tid = 0; tid < this.threadCount; ++tid) {
            assert.commandWorked(mutexDb[data.CRUDMutex].insert({tid: tid, mutex: 0}));
        }
        // Forcefully create a collection in the FSM provided database in order to never encounter a database not found error.
        assert.commandWorked(db[collName].createIndex({x: 1}));
    };

    let teardown = function (db, collName, cluster) {
        TestData.implicitRetryDdlOnConflictWithMigration =
            this.originalImplicitRetryDdlOnConflictWithMigration;
    };

    return {
        threadCount: 12,
        iterations: 64,
        startState: "init",
        states: states,
        transitions: uniformDistTransitions(states),
        data: data,
        setup: setup,
        teardown: teardown,
        passConnectionCache: true,
    };
})();
