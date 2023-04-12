'use strict';

/**
 * Runs updateOne, deleteOne, and findAndModify without shard key against a sharded cluster.
 *
 * @tags: [
 *  featureFlagUpdateOneWithoutShardKey,
 *  requires_fcv_70,
 *  requires_sharding,
 *  uses_transactions,
 * ]
 */

load('jstests/concurrency/fsm_libs/extend_workload.js');

// This workload does not make use of random moveChunks, but other workloads that extend this base
// workload may.
load('jstests/concurrency/fsm_workloads/random_moveChunk_base.js');  // for $config
load('jstests/concurrency/fsm_workload_helpers/balancer.js');

var $config = extendWorkload($config, function($config, $super) {
    $config.threadCount = 10;
    $config.iterations = 10;
    $config.startState = "init";  // Inherited from random_moveChunk_base.js.
    $config.data.partitionSize = 50;
    $config.data.secondaryDocField = 'y';
    $config.data.tertiaryDocField = 'a';
    $config.data.runningWithStepdowns =
        TestData.runningWithConfigStepdowns || TestData.runningWithShardStepdowns;

    /**
     * Returns a random integer between min (inclusive) and max (inclusive).
     */
    $config.data.generateRandomInt = function generateRandomInt(min, max) {
        return Math.floor(Math.random() * (max - min + 1)) + min;
    };

    /**
     * Returns a random boolean.
     */
    $config.data.generateRandomBool = function generateRandomBool() {
        return Math.random() > 0.5;
    };

    /**
     * Generates a random document.
     */
    $config.data.generateRandomDocument = function generateRandomDocument(tid, partition) {
        const val = this.generateRandomInt(partition.lower, partition.upper - 1);
        return {
            _id: UUID(),
            tid: tid,
            [this.defaultShardKeyField]: val,
            [this.secondaryDocField]: val
        };
    };

    /**
     * Does an initial populating of the collection with random documents.
     */
    $config.data.insertInitialDocuments = function insertInitialDocuments(db, collName, tid) {
        const ns = db.getName() + "." + collName;
        const partition = this.makePartition(ns, tid, this.partitionSize);
        let bulk = db.getCollection(collName).initializeUnorderedBulkOp();
        for (let i = 0; i < this.partitionSize; ++i) {
            const doc = this.generateRandomDocument(tid, partition);
            bulk.insert(doc);
        }
        assert.commandWorked(bulk.execute());
    };

    /**
     * Randomly generates a query that cannot uniquely target a chunk that spans the allotted
     * partition for this thread id. The chunks could be distributed among multiple shards, which
     * mean the query could target a variable number of shards.
     */
    $config.data.generateRandomQuery = function generateRandomQuery(db, collName) {
        const queryType = this.generateRandomInt(0, 3);
        if (queryType === 0 /* Range query on shard key field. */) {
            return {
                [this.defaultShardKeyField]:
                    {$gte: this.partition.lower, $lte: this.partition.upper - 1}
            };
        } else if (queryType === 1 /* Range query on non shard key field. */) {
            return {
                [this.secondaryDocField]:
                    {$gte: this.partition.lower, $lte: this.partition.upper - 1}
            };
        } else if (queryType === 2 /* Equality query on a field that does not exist */) {
            return {[this.tertiaryDocField]: {$eq: this.generateRandomInt(0, 500)}, tid: this.tid};
        } else { /* Query any document in the partition. */
            return {tid: this.tid};
        }
    };

    /**
     * Randomly generates and runs an update operator document update, replacement update, or an
     * aggregation pipeline update.
     */
    $config.data.generateAndRunRandomUpdateOp = function generateAndRunRandomUpdateOp(db,
                                                                                      collName) {
        const query = this.generateRandomQuery(db, collName);
        const newValue = this.generateRandomInt(this.partition.lower, this.partition.upper - 1);
        const updateType = this.generateRandomInt(0, 2);
        const doShardKeyUpdate = this.generateRandomInt(0, 1);
        const doUpsert = this.generateRandomBool();

        // Used for validation after running the write operation.
        const containsMatchedDocs = db[collName].findOne(query) != null;

        let res;
        try {
            if (updateType === 0 /* Update operator document */) {
                const update = {
                    [doShardKeyUpdate ? this.defaultShardKeyField : this.secondaryDocField]:
                        newValue
                };
                res = db[collName].updateOne(query, {$set: update}, {upsert: doUpsert});
            } else if (updateType === 1 /* Replacement Update */) {
                // Always including a shard key update for replacement documents in order to keep
                // the new document within the current thread's partition.
                res = db[collName].replaceOne(query,
                                              {
                                                  [this.defaultShardKeyField]: newValue,
                                                  [this.secondaryDocField]: newValue,
                                                  tid: this.tid
                                              },
                                              {upsert: doUpsert});
            } else { /* Aggregation pipeline update */
                const update = {
                    [doShardKeyUpdate ? this.defaultShardKeyField : this.secondaryDocField]:
                        newValue
                };

                // The $unset will result in a no-op since 'z' is not a field populated in any of
                // the documents.
                res = db[collName].updateOne(
                    query, [{$set: update}, {$unset: "z"}], {upsert: doUpsert});
            }
        } catch (err) {
            if (this.shouldSkipWriteResponseValidation(err)) {
                return;
            }
            throw err;
        }

        assert.commandWorked(res);

        if (containsMatchedDocs) {
            assert.eq(res.matchedCount, 1, query);
        } else {
            assert.eq(res.matchedCount, 0, res);

            if (doUpsert) {
                assert.neq(res.upsertedId, null, res);
                assert.eq(db[collName].find({"_id": res.upsertedId}).itcount(), 1);

                // Clean up, remove upserted document.
                assert.commandWorked(db[collName].deleteOne({"_id": res.upsertedId}));
            }
        }

        assert.contains(res.modifiedCount, [0, 1], res);

        // In case the modification results in no change to the document, matched may be higher
        // than modified.
        assert.gte(res.matchedCount, res.modifiedCount, res);
    };

    /**
     * Checks the response of a write. If we have a write error, return true if we should skip write
     * response validation for an acceptable error, false otherwise.
     */
    $config.data.shouldSkipWriteResponseValidation = function shouldSkipWriteResponseValidation(
        res) {
        let acceptableErrors = [
            ErrorCodes.DuplicateKey,
            ErrorCodes.IllegalOperation,
            ErrorCodes.LockTimeout,
            ErrorCodes.IncompleteTransactionHistory,
            ErrorCodes.NoSuchTransaction,
            ErrorCodes.StaleConfig,
        ];

        // If we're running in a stepdown suite, then attempting to update the shard key may
        // interact with stepdowns and transactions to cause the following errors. We only expect
        // these errors in stepdown suites and not in other suites, so we surface them to the test
        // runner in other scenarios.
        const stepdownErrors = [ErrorCodes.ConflictingOperationInProgress];

        if (this.runningWithStepdowns) {
            acceptableErrors.push(...stepdownErrors);
        }

        const duplicateKeyInChangeShardKeyMsg = "Failed to update document's shard key field";
        const wouldChangeOwningShardMsg =
            "Must run update to shard key field in a multi-statement transaction";
        const otherErrorsInChangeShardKeyMsg = "was converted into a distributed transaction";
        const failureInRetryableWriteToTxnConversionMsg =
            "Cannot retry a retryable write that has been converted";

        if (res.code && (res.code !== ErrorCodes.OK)) {
            if (acceptableErrors.includes(res.code)) {
                const msg = res.errmsg ? res.errmsg : res.message;

                // This duplicate key error is only acceptable if it's a document shard key
                // change during a concurrent migration.
                if (res.code === ErrorCodes.DuplicateKey) {
                    if (!msg.includes(duplicateKeyInChangeShardKeyMsg)) {
                        return false;
                    }
                }

                // This is a possible transient transaction error issue that could occur with
                // concurrent moveChunks and transactions (if we happen to run a
                // WouldChangeOwningShard update).
                if (res.code === ErrorCodes.LockTimeout || res.code === ErrorCodes.StaleConfig ||
                    res.code === ErrorCodes.ConflictingOperationInProgress) {
                    if (!msg.includes(otherErrorsInChangeShardKeyMsg)) {
                        return false;
                    }
                }

                // In the current implementation, retrying a retryable write that was converted into
                // a distributed transaction should fail with IncompleteTransactionHistory.
                if (res.code === ErrorCodes.IncompleteTransactionHistory) {
                    if (!msg.includes(failureInRetryableWriteToTxnConversionMsg)) {
                        return false;
                    }
                }

                // TODO: SERVER-67429 Remove this since we can run in all configurations.
                // If we have a WouldChangeOwningShard update and we aren't running as a retryable
                // write or in a transaction, then this is an acceptable error.
                if (res.code === ErrorCodes.IllegalOperation) {
                    if (!msg.includes(wouldChangeOwningShardMsg)) {
                        return false;
                    }
                }

                // If we're here that means the remaining acceptable errors must be
                // TransientTransactionErrors.
                if (res.errorLabels && !res.errorLabels.includes("TransientTransactionError")) {
                    return false;
                }
                return true;
            } else {
                return false;
            }
        } else {
            // We got an OK response from running the command.
            return false;
        }
    };

    /**
     * Randomly generates and runs either a findAndModify update or a findAndModify remove.
     */
    $config.data.generateAndRunRandomFindAndModifyOp = function generateAndRunRandomFindAndModifyOp(
        db, collName) {
        const query = this.generateRandomQuery(db, collName);

        // Used for validation after running the write operation.
        const containsMatchedDocs = db[collName].findOne(query) != null;

        // Only test sort when there are matching documents in the collection.
        const doSort = containsMatchedDocs && this.generateRandomBool();
        let sortDoc, sortVal;

        // If sorting, ensure that the correct document is modified.
        if (doSort) {
            sortVal = {[this.secondaryDocField]: this.generateRandomInt(0, 1) === 0 ? -1 : 1};
            sortDoc = db[collName].find(query).sort(sortVal)[0];
        }

        let res;
        const findAndModifyType = this.generateRandomInt(0, 1);
        if (findAndModifyType === 0 /* Update */) {
            const newValue = this.generateRandomInt(this.partition.lower, this.partition.upper - 1);
            const updateType = this.generateRandomInt(0, 2);
            const doShardKeyUpdate = this.generateRandomInt(0, 1);
            const doUpsert = this.generateRandomBool();
            const cmdObj = {
                findAndModify: collName,
                query: query,
                upsert: doUpsert,
            };
            Object.assign(cmdObj, doSort && {sort: sortVal});

            if (updateType === 0 /* Update operator document */) {
                const update = {
                    [doShardKeyUpdate ? this.defaultShardKeyField : this.secondaryDocField]:
                        newValue
                };
                cmdObj.update = {$set: update};
                res = db.runCommand(cmdObj);
            } else if (updateType === 1 /* Replacement Update */) {
                // Always including a shard key update for replacement documents in order to
                // keep the new document within the current thread's partition.
                cmdObj.update = {
                    [this.defaultShardKeyField]: newValue,
                    [this.secondaryDocField]: newValue,
                    tid: this.tid
                };
                res = db.runCommand(cmdObj);
            } else { /* Aggregation pipeline update */
                const update = {
                    [doShardKeyUpdate ? this.defaultShardKeyField : this.secondaryDocField]:
                        newValue
                };

                // The $unset will result in a no-op since 'z' is not a field populated in any
                // of the documents.
                cmdObj.update = [{$set: update}, {$unset: "z"}];
                res = db.runCommand(cmdObj);
            }

            if (this.shouldSkipWriteResponseValidation(res)) {
                return;
            }

            assert.commandWorked(res);
            if (containsMatchedDocs) {
                assert.eq(res.lastErrorObject.n, 1, res);
                assert.eq(res.lastErrorObject.updatedExisting, true, res);
            } else if (doUpsert) {
                assert.eq(res.lastErrorObject.n, 1, res);
                assert.eq(res.lastErrorObject.updatedExisting, false, res);
                assert.neq(res.lastErrorObject.upserted, null, res);
                assert.eq(db[collName].find({"_id": res.lastErrorObject.upserted}).itcount(), 1);

                // Clean up, remove upserted document.
                assert.commandWorked(db[collName].deleteOne({"_id": res.lastErrorObject.upserted}));
            } else {
                assert.eq(res.lastErrorObject.n, 0, res);
                assert.eq(res.lastErrorObject.updatedExisting, false, res);
            }
        } else { /* Remove */
            const numMatchedDocsBefore = db[collName].find(query).itcount();
            const cmdObj = {
                findAndModify: collName,
                query: query,
                remove: true,
            };
            if (doSort) {
                cmdObj.sort = sortVal;
            }
            res = assert.commandWorked(db.runCommand(cmdObj));

            const numMatchedDocsAfter = db[collName].find(query).itcount();

            if (numMatchedDocsBefore > 0) {
                assert.eq(res.lastErrorObject.n, 1, res);
                assert.eq(numMatchedDocsAfter, numMatchedDocsBefore - 1);
            } else {
                assert.eq(res.lastErrorObject.n, 0, res);

                // The count should both be 0.
                assert.eq(numMatchedDocsAfter, numMatchedDocsBefore);
            }
        }

        if (doSort) {
            // Ensure correct document was modified by comparing sort field of the sortDoc and
            // response image.
            assert.eq(sortDoc.secondaryDocField, res.value.secondaryDocField, res);
        }
    };

    $config.states.updateOne = function updateOne(db, collName, connCache) {
        jsTestLog("Running updateOne state");
        this.generateAndRunRandomUpdateOp(db, collName);
    };

    $config.states.deleteOne = function deleteOne(db, collName, connCache) {
        jsTestLog("Running deleteOne state");
        const query = this.generateRandomQuery(db, collName);

        // Used for validation after running the write operation.
        const containsMatchedDocs = db[collName].findOne(query) != null;
        const numMatchedDocsBefore = db[collName].find(query).itcount();

        let res = assert.commandWorked(db[collName].deleteOne(query));

        const numMatchedDocsAfter = db[collName].find(query).itcount();

        if (containsMatchedDocs) {
            assert.eq(res.deletedCount, 1, res);
            assert.eq(numMatchedDocsAfter, numMatchedDocsBefore - 1);
        } else {
            assert.eq(res.deletedCount, 0, res);

            // The count should both be 0.
            assert.eq(numMatchedDocsAfter, numMatchedDocsBefore);
        }
    };

    $config.states.findAndModify = function findAndModify(db, collName, connCache) {
        jsTestLog("Running findAndModify state");
        this.generateAndRunRandomFindAndModifyOp(db, collName);
    };

    $config.setup = function setup(db, collName, cluster) {
        const nss = db + "." + collName;

        // Disallow balancing 'ns' during $setup so it does not interfere with the splits.
        BalancerHelper.disableBalancerForCollection(db, nss);
        BalancerHelper.joinBalancerRound(db);

        const shards = Object.keys(cluster.getSerializedCluster().shards);
        ChunkHelper.moveChunk(
            db,
            collName,
            [{[this.defaultShardKeyField]: MinKey}, {[this.defaultShardKeyField]: MaxKey}],
            shards[0]);

        for (let tid = 0; tid < this.threadCount; ++tid) {
            const partition = this.makePartition(nss, tid, this.partitionSize);

            // Create two chunks for the partition assigned to this thread:
            // [partition.lower, partition.mid] and [partition.mid, partition.upper].

            // The lower bound for a low chunk partition is minKey, so a split is not necessary.
            if (!partition.isLowChunk) {
                assert.commandWorked(db.adminCommand(
                    {split: nss, middle: {[this.defaultShardKeyField]: partition.lower}}));
            }

            assert.commandWorked(db.adminCommand(
                {split: nss, middle: {[this.defaultShardKeyField]: partition.mid}}));

            // Move one of the two chunks assigned to this thread to one of the other shards.
            ChunkHelper.moveChunk(
                db,
                collName,
                [
                    {[this.defaultShardKeyField]: partition.isLowChunk ? MinKey : partition.lower},
                    {[this.defaultShardKeyField]: partition.mid}
                ],
                shards[this.generateRandomInt(1, shards.length - 1)]);
        }

        // There isn't a way to determine what the thread ids are in setup phase so just assume
        // that they are [0, 1, ..., this.threadCount-1].
        for (let tid = 0; tid < this.threadCount; ++tid) {
            this.insertInitialDocuments(db, collName, tid);
        }

        // Allow balancing 'nss' again.
        BalancerHelper.enableBalancerForCollection(db, nss);
    };

    $config.transitions = {
        init: {updateOne: 0.3, deleteOne: 0.3, findAndModify: 0.4},
        updateOne: {updateOne: 0.3, deleteOne: 0.3, findAndModify: 0.4},
        deleteOne: {updateOne: 0.3, deleteOne: 0.3, findAndModify: 0.4},
        findAndModify: {updateOne: 0.3, deleteOne: 0.3, findAndModify: 0.4}
    };

    return $config;
});
