/**
 * Runs updateOne, deleteOne, and findAndModify without shard key against a sharded cluster. For
 * validation purposes, this test is set up such that each thread only modifies documents in its own
 * partition, so there won't be concurrent modifications to the same document across threads.
 * However, writes may conflict with move chunks.
 *
 * @tags: [
 *  # updateOne with a sort option is supported in 8.0
 *  requires_fcv_81,
 *  requires_sharding,
 *  uses_transactions,
 *  assumes_stable_shard_list,
 * ]
 */

import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/sharded_partitioned/crud_base_partitioned.js";

export const $config = extendWorkload($baseConfig, function ($config, $super) {
    $config.threadCount = 10;
    $config.iterations = 50;
    $config.startState = "init"; // Inherited from crud_base_partitioned.js.
    $config.data.partitionSize = 100;
    $config.data.secondaryDocField = "y";
    $config.data.idField = "_id";
    $config.data.tertiaryDocField = "tertiaryField";
    $config.data.runningWithStepdowns = TestData.runningWithConfigStepdowns || TestData.runningWithShardStepdowns;

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
    $config.data.generateRandomDocument = function generateRandomDocument(tid, partition, idVal) {
        const val = this.generateRandomInt(partition.lower, partition.upper - 1);
        return {
            _id: idVal,
            tid: tid,
            [this.defaultShardKeyField]: val,
            [this.secondaryDocField]: val,
        };
    };

    /**
     * Does an initial populating of the collection with random documents.
     */
    $config.data.insertInitialDocuments = function insertInitialDocuments(db, collName, tid) {
        const ns = db.getName() + "." + collName;
        const partition = this.makePartition(ns, tid, this.partitionSize);
        let bulk = db.getCollection(collName).initializeUnorderedBulkOp();
        let val = partition.lower;
        for (let i = 0; i < this.partitionSize; ++i) {
            const doc = this.generateRandomDocument(tid, partition, val);
            bulk.insert(doc);
            val++;
        }
        assert.commandWorked(bulk.execute());
    };

    /**
     * Randomly generates a query that cannot uniquely target a chunk that spans the allotted
     * partition for this thread id. The chunks could be distributed among multiple shards, which
     * mean the query could target a variable number of shards.
     */
    $config.data.generateRandomQuery = function generateRandomQuery() {
        const queryType = this.generateRandomInt(0, 3);
        if (queryType === 0 /* Range query on shard key field. */) {
            return {
                [this.defaultShardKeyField]: {$gte: this.partition.lower, $lte: this.partition.upper - 1},
            };
        } else if (queryType === 1 /* Range query on non shard key field. */) {
            return {
                [this.secondaryDocField]: {$gte: this.partition.lower, $lte: this.partition.upper - 1},
            };
        } else if (queryType === 2 /* Equality query on a field that does not exist */) {
            return {[this.tertiaryDocField]: {$eq: this.generateRandomInt(0, 500)}, tid: this.tid};
        } else {
            /* Query any document in the partition. */
            return {tid: this.tid};
        }
    };

    /**
     * Sorts documents by sortVal and returns an array of the _id fields of documents that are first
     * in the sort order.
     */
    $config.data.returnDocsThatSortFirst = function returnDocsThatSortFirst(db, collName, query, options) {
        // If sorting, ensure that the correct document is modified. Save the _id values of the
        // documents that come first in the sort order, and validate that a document that comes
        // first in the sort order is correctly applied the update.
        const sortVal = {[this.secondaryDocField]: this.generateRandomInt(0, 1) === 0 ? -1 : 1};
        options.sort = sortVal;

        // Need to specify batch count that is larger than the total number of records to
        // prevent getMore command from being issued since stepdown suites ban it.
        const docs = db[collName].find(query).sort(sortVal).batchSize(1e6).toArray();

        // A previous update op with a query on a non-existent tertiary field, doUpsert = true,
        // and doShardKeyUpdate = true could lead to a document with no secondaryDocField value
        // getting inserted into the collection. This would sort first in an ascending sort
        // because missing/null comes before integers in the BSON comparison order. Since we
        // sort on the secondaryDocField and assume that it exists to validate the sort, we will
        // skip write response validation if it doesn't exist for ease of testing.
        const secondaryDocFieldVal = docs[0][this.secondaryDocField];
        if (secondaryDocFieldVal == undefined) {
            return undefined;
        }

        const docsThatSortFirstIds = db[collName]
            .find({[this.secondaryDocField]: secondaryDocFieldVal}, {[this.idField]: 1})
            .toArray();

        return docsThatSortFirstIds;
    };

    /**
     * Validate that for an update with a sort, the correct document (one that sorts first) was
     * updated.
     */
    $config.data.validateSortedUpdate = function validateSortedUpdate(
        db,
        collName,
        update,
        doShardKeyUpdate,
        docsThatSortFirstIds,
    ) {
        let correctDocModified = false;
        const sortField = doShardKeyUpdate ? this.defaultShardKeyField : this.secondaryDocField;

        for (let i = 0; i < docsThatSortFirstIds.length; i++) {
            let docSort = db[collName].find(docsThatSortFirstIds[i]).toArray()[0];
            if (docSort[sortField] == update[sortField]) {
                correctDocModified = true;
            }
        }
        assert(correctDocModified);
    };

    /**
     * Randomly generates and runs an update operator document update, replacement update,
     * or an aggregation pipeline update.
     */
    $config.data.generateAndRunRandomUpdateOp = function generateAndRunRandomUpdateOp(db, collName) {
        const query = this.generateRandomQuery();
        const newValue = this.generateRandomInt(this.partition.lower, this.partition.upper - 1);
        const updateType = this.generateRandomInt(0, 2);
        const doShardKeyUpdate = this.generateRandomInt(0, 1);
        const doUpsert = this.generateRandomBool();

        // Used for validation after running the write operation.
        const containsMatchedDocs = db[collName].findOne(query) != null;

        // Only test sort when there are matching documents in the collection. We do not test sort
        // for replacement updates as replaceOne does not support a sort parameter.
        const doSort = containsMatchedDocs && updateType !== 1 && this.generateRandomBool();
        let docsThatSortFirstIds = new Array();

        let options = {upsert: doUpsert};
        if (doSort) {
            docsThatSortFirstIds = this.returnDocsThatSortFirst(db, collName, query, options);
            if (docsThatSortFirstIds == undefined) {
                return;
            }
        }

        jsTestLog(
            "updateOne state running with the following parameters: \n" +
                "query: " +
                tojson(query) +
                "\n" +
                "updateType: " +
                updateType +
                "\n" +
                "doShardKeyUpdate: " +
                doShardKeyUpdate +
                "\n" +
                "doUpsert: " +
                doUpsert +
                "\n" +
                "doSort: " +
                doSort +
                "\n" +
                "containsMatchedDocs: " +
                containsMatchedDocs,
        );

        let res;
        let update;
        try {
            if (updateType === 0 /* Update operator document */) {
                update = {
                    [doShardKeyUpdate ? this.defaultShardKeyField : this.secondaryDocField]: newValue,
                };
                res = db[collName].updateOne(query, {$set: update}, options);
            } else if (updateType === 1 /* Replacement Update */) {
                // Always including a shard key update for replacement documents in order to keep
                // the new document within the current thread's partition.
                res = db[collName].replaceOne(
                    query,
                    {
                        [this.defaultShardKeyField]: newValue,
                        [this.secondaryDocField]: newValue,
                        tid: this.tid,
                    },
                    options,
                );
            } else {
                /* Aggregation pipeline update */
                update = {
                    [doShardKeyUpdate ? this.defaultShardKeyField : this.secondaryDocField]: newValue,
                };

                // The $unset will result in a no-op since 'z' is not a field populated in any of
                // the documents.
                res = db[collName].updateOne(query, [{$set: update}, {$unset: "z"}], options);
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
                assert.eq(db[collName].countDocuments({"_id": res.upsertedId}), 1);

                // Clean up, remove upserted document.
                assert.commandWorked(db[collName].deleteOne({"_id": res.upsertedId}));
            }
        }

        assert.contains(res.modifiedCount, [0, 1], res);

        // In case the modification results in no change to the document, matched may be higher
        // than modified.
        assert.gte(res.matchedCount, res.modifiedCount, res);

        if (doSort) {
            this.validateSortedUpdate(db, collName, update, doShardKeyUpdate, docsThatSortFirstIds);
        }
    };

    /**
     * Randomly generates and runs an update operator document update without shard key with ID,
     * replacement update, or an aggregation pipeline update.
     */
    $config.data.generateAndRunRandomUpdateOpWithId = function generateAndRunRandomUpdateOpWithId(db, collName) {
        const query = {
            _id: {
                $eq: this.generateRandomInt(
                    this.partition.lower - this.partitionSize / 4,
                    this.partition.upper + this.partitionSize / 4,
                ),
            },
            tid: this.tid,
        };
        const newValue = this.generateRandomInt(this.partition.lower, this.partition.upper - 1);
        const updateType = this.generateRandomInt(0, 1);

        // Used for validation after running the write operation.
        const containsMatchedDocs = db[collName].findOne(query) != null;

        jsTestLog(
            "updateOneWithId state running with the following parameters: \n" +
                "query: " +
                tojson(query) +
                "\n" +
                "updateType: " +
                updateType +
                "\n" +
                "containsMatchedDocs: " +
                containsMatchedDocs,
        );

        // If the suite runs this function as a txn with retries already as in
        // concurrency_sharded_multi_stmt_txn suites, we skip creating a retryable writes session.
        // In this case the write is run as WriteType::Ordinary in a txn. In all other cases, we
        // create a new retryable writes session to test WriteType::WithoutShardKeyWithId as
        // currently we only categorize non transactional retryable writes into this write type.
        let session;
        let collection;
        if (!db.getSession()._serverSession.isTxnActive()) {
            session = db.getMongo().startSession({retryWrites: true});
            collection = session.getDatabase(db.getName()).getCollection(collName);
        } else {
            collection = db[collName];
        }

        let res;
        if (updateType === 0 /* Update operator document */) {
            const update = {[this.secondaryDocField]: newValue};
            res = collection.updateOne(query, {$set: update});
        } else {
            /* Aggregation pipeline update */
            const update = {[this.secondaryDocField]: newValue};
            res = collection.updateOne(query, [{$set: update}]);
        }
        assert.commandWorked(res);

        if (containsMatchedDocs) {
            assert.eq(res.matchedCount, 1, query);
        } else {
            assert.eq(res.matchedCount, 0, res);
        }

        assert.contains(res.modifiedCount, [0, 1], res);

        // In case the modification results in no change to the document, matched may be higher
        // than modified.
        assert.gte(res.matchedCount, res.modifiedCount, res);
        if (session) {
            session.endSession();
        }
    };

    /**
     * Checks the response of a write. If we have a write error, return true if we should skip write
     * response validation for an acceptable error, false otherwise.
     */
    $config.data.shouldSkipWriteResponseValidation = function shouldSkipWriteResponseValidation(res) {
        let acceptableErrors = [
            ErrorCodes.DuplicateKey,
            ErrorCodes.IllegalOperation,
            ErrorCodes.LockTimeout,
            ErrorCodes.IncompleteTransactionHistory,
            ErrorCodes.MigrationConflict,
            ErrorCodes.NoSuchTransaction,
            ErrorCodes.StaleConfig,
            ErrorCodes.ShardCannotRefreshDueToLocksHeld,
            ErrorCodes.WriteConflict,
            ErrorCodes.SnapshotUnavailable,
            ErrorCodes.ExceededTimeLimit,
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
        const wouldChangeOwningShardMsg = "Must run update to shard key field in a multi-statement transaction";
        const otherErrorsInChangeShardKeyMsg = "was converted into a distributed transaction";
        const failureInRetryableWriteToTxnConversionMsg = "Cannot retry a retryable write that has been converted";

        if (res.code && res.code !== ErrorCodes.OK) {
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
                // concurrent moveChunks and/or reshardings and transactions (if we happen to run a
                // WouldChangeOwningShard update).
                if (
                    res.code === ErrorCodes.LockTimeout ||
                    res.code === ErrorCodes.StaleConfig ||
                    res.code === ErrorCodes.ConflictingOperationInProgress ||
                    res.code === ErrorCodes.ShardCannotRefreshDueToLocksHeld ||
                    res.code == ErrorCodes.WriteConflict ||
                    res.code == ErrorCodes.SnapshotUnavailable ||
                    res.code == ErrorCodes.ExceededTimeLimit
                ) {
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
    $config.data.generateAndRunRandomFindAndModifyOp = function generateAndRunRandomFindAndModifyOp(db, collName) {
        const query = this.generateRandomQuery();

        // Used for validation after running the write operation.
        const containsMatchedDocs = db[collName].findOne(query) != null;

        // Only test sort when there are matching documents in the collection.
        const doSort = containsMatchedDocs && this.generateRandomBool();
        let sortDoc, sortVal;

        // If sorting, ensure that the correct document is modified.
        if (doSort) {
            sortVal = {[this.secondaryDocField]: this.generateRandomInt(0, 1) === 0 ? -1 : 1};
            // Need to specify batch count that is larger than the total number of records to
            // prevent getMore command from being issued since stepdown suites ban it.
            sortDoc = db[collName].find(query).sort(sortVal).batchSize(1e6)[0];
        }

        let res;
        const findAndModifyType = this.generateRandomInt(0, 1);
        if (findAndModifyType === 0 /* Update */) {
            const newValue = this.generateRandomInt(this.partition.lower, this.partition.upper - 1);
            const updateType = this.generateRandomInt(0, 2);
            const doShardKeyUpdate = this.generateRandomInt(0, 1);
            const doUpsert = this.generateRandomBool();

            jsTestLog(
                "findAndModifyUpdate state running with the following parameters: \n" +
                    "query: " +
                    tojson(query) +
                    "\n" +
                    "updateType: " +
                    updateType +
                    "\n" +
                    "doShardKeyUpdate: " +
                    doShardKeyUpdate +
                    "\n" +
                    "doUpsert: " +
                    doUpsert +
                    "\n" +
                    "doSort: " +
                    doSort +
                    "\n" +
                    "containsMatchedDocs: " +
                    containsMatchedDocs,
            );

            const cmdObj = {
                findAndModify: collName,
                query: query,
                upsert: doUpsert,
            };
            Object.assign(cmdObj, doSort && {sort: sortVal});

            if (updateType === 0 /* Update operator document */) {
                const update = {
                    [doShardKeyUpdate ? this.defaultShardKeyField : this.secondaryDocField]: newValue,
                };
                cmdObj.update = {$set: update};
                res = db.runCommand(cmdObj);
            } else if (updateType === 1 /* Replacement Update */) {
                // Always including a shard key update for replacement documents in order to
                // keep the new document within the current thread's partition.
                cmdObj.update = {
                    [this.defaultShardKeyField]: newValue,
                    [this.secondaryDocField]: newValue,
                    tid: this.tid,
                };
                res = db.runCommand(cmdObj);
            } else {
                /* Aggregation pipeline update */
                const update = {
                    [doShardKeyUpdate ? this.defaultShardKeyField : this.secondaryDocField]: newValue,
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
                assert.eq(db[collName].countDocuments({"_id": res.lastErrorObject.upserted}), 1);

                // Clean up, remove upserted document.
                assert.commandWorked(db[collName].deleteOne({"_id": res.lastErrorObject.upserted}));
            } else {
                assert.eq(res.lastErrorObject.n, 0, res);
                assert.eq(res.lastErrorObject.updatedExisting, false, res);
            }
        } else {
            /* Remove */
            const numMatchedDocsBefore = db[collName].countDocuments(query);
            const cmdObj = {
                findAndModify: collName,
                query: query,
                remove: true,
            };
            if (doSort) {
                cmdObj.sort = sortVal;
            }

            jsTestLog(
                "findAndModifyDelete state running with the following parameters: \n" +
                    "query: " +
                    tojson(query) +
                    "\n" +
                    "numMatchedDocsBefore: " +
                    numMatchedDocsBefore +
                    "\n" +
                    "containsMatchedDocs: " +
                    containsMatchedDocs,
            );

            res = assert.commandWorked(db.runCommand(cmdObj));

            const numMatchedDocsAfter = db[collName].countDocuments(query);

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

    $config.states.init = function init(db, collName, connCache) {
        $super.states.init.apply(this, arguments);
    };

    $config.states.updateOne = function updateOne(db, collName, connCache) {
        jsTestLog("Running updateOne state");
        this.generateAndRunRandomUpdateOp(db, collName);
        jsTestLog("Finished updateOne state");
    };

    $config.states.updateOneWithId = function updateOneWithId(db, collName, connCache) {
        jsTestLog("Running updateOneWithId state");
        this.generateAndRunRandomUpdateOpWithId(db, collName);
        jsTestLog("Finished updateOneWithId state");
    };

    $config.states.deleteOne = function deleteOne(db, collName, connCache) {
        jsTestLog("Running deleteOne state");
        const query = this.generateRandomQuery();

        // Used for validation after running the write operation.
        const containsMatchedDocs = db[collName].findOne(query) != null;
        const numMatchedDocsBefore = db[collName].countDocuments(query);

        jsTestLog(
            "deleteOne state running with query: " +
                tojson(query) +
                "\n" +
                "containsMatchedDocs: " +
                containsMatchedDocs +
                "\n" +
                "numMatchedDocsBefore: " +
                numMatchedDocsBefore,
        );

        let res = assert.commandWorked(db[collName].deleteOne(query));

        const numMatchedDocsAfter = db[collName].countDocuments(query);

        if (containsMatchedDocs) {
            assert.eq(res.deletedCount, 1, res);
            assert.eq(numMatchedDocsAfter, numMatchedDocsBefore - 1);
        } else {
            assert.eq(res.deletedCount, 0, res);

            // The count should both be 0.
            assert.eq(numMatchedDocsAfter, numMatchedDocsBefore);
        }
        jsTestLog("Finished deleteOne state");
    };

    $config.states.deleteOneWithId = function deleteOneWithId(db, collName, connCache) {
        jsTestLog("Running deleteOneWithId state");
        const query = {
            _id: {
                $eq: this.generateRandomInt(
                    this.partition.lower - this.partitionSize / 4,
                    this.partition.upper + this.partitionSize / 4,
                ),
            },
            tid: this.tid,
        };

        // If the suite runs this function as a txn with retries already as in
        // concurrency_sharded_multi_stmt_txn suites, we skip creating a retryable writes session.
        // In this case the write is run as WriteType::Ordinary in a txn. In all other cases, we
        // create a new retryable writes session to test WriteType::WithoutShardKeyWithId as
        // currently we only categorize non transactional retryable writes into this write type.
        let session;
        let collection;
        if (!db.getSession()._serverSession.isTxnActive()) {
            session = db.getMongo().startSession({retryWrites: true});
            collection = session.getDatabase(db.getName()).getCollection(collName);
        } else {
            collection = db[collName];
        }

        // Used for validation after running the write operation.
        const containsMatchedDocs = collection.findOne(query) != null;
        const numMatchedDocsBefore = collection.countDocuments(query);

        jsTestLog(
            "deleteOneWithId state running with query: " +
                tojson(query) +
                "\n" +
                "containsMatchedDocs: " +
                containsMatchedDocs +
                "\n" +
                "numMatchedDocsBefore: " +
                numMatchedDocsBefore,
        );

        let res = assert.commandWorked(collection.deleteOne(query));

        const numMatchedDocsAfter = collection.countDocuments(query);

        if (containsMatchedDocs) {
            assert.eq(res.deletedCount, 1, res);
            assert.eq(numMatchedDocsAfter, numMatchedDocsBefore - 1);
        } else {
            assert.eq(res.deletedCount, 0, res);

            // The count should both be 0.
            assert.eq(numMatchedDocsAfter, numMatchedDocsBefore);
        }
        if (session) {
            session.endSession();
        }
        jsTestLog("Finished deleteOneWithId state");
    };

    $config.states.findAndModify = function findAndModify(db, collName, connCache) {
        jsTestLog("Running findAndModify state");
        this.generateAndRunRandomFindAndModifyOp(db, collName);
        jsTestLog("Finished findAndModify state");
    };

    $config.setup = function setup(db, collName, cluster) {
        // There isn't a way to determine what the thread ids are in setup phase so just assume
        // that they are [0, 1, ..., this.threadCount-1].
        for (let tid = 0; tid < this.threadCount; ++tid) {
            this.insertInitialDocuments(db, collName, tid);
        }
    };

    $config.transitions = {
        init: {
            updateOne: 0.175,
            deleteOne: 0.175,
            updateOneWithId: 0.175,
            deleteOneWithId: 0.175,
            findAndModify: 0.3,
        },
        updateOne: {
            updateOne: 0.175,
            deleteOne: 0.175,
            updateOneWithId: 0.175,
            deleteOneWithId: 0.175,
            findAndModify: 0.3,
        },
        deleteOne: {
            updateOne: 0.175,
            deleteOne: 0.175,
            updateOneWithId: 0.175,
            deleteOneWithId: 0.175,
            findAndModify: 0.3,
        },
        updateOneWithId: {
            updateOne: 0.175,
            deleteOne: 0.175,
            updateOneWithId: 0.175,
            deleteOneWithId: 0.175,
            findAndModify: 0.3,
        },
        deleteOneWithId: {
            updateOne: 0.175,
            deleteOne: 0.175,
            updateOneWithId: 0.175,
            deleteOneWithId: 0.175,
            findAndModify: 0.3,
        },
        findAndModify: {
            updateOne: 0.175,
            deleteOne: 0.175,
            updateOneWithId: 0.175,
            deleteOneWithId: 0.175,
            findAndModify: 0.3,
        },
    };

    return $config;
});
