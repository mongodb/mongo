'use strict';

/**
 * Performs updates that will change a document's shard key while migrating chunks. Uses both
 * retryable writes and multi-statement transactions.
 *
 * @tags: [
 *  requires_sharding,
 *  assumes_balancer_off,
 *  requires_non_retryable_writes,
 *  uses_transactions,
 * ]
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');
load('jstests/concurrency/fsm_workloads/random_moveChunk_base.js');
load('jstests/concurrency/fsm_workload_helpers/auto_retry_transaction.js');

var $config = extendWorkload($config, function($config, $super) {
    $config.threadCount = 5;
    $config.iterations = 50;

    // Number of documents per partition. Note that there is one chunk per partition and one
    // partition per thread.
    $config.data.partitionSize = 200;

    // The counter values associated with each owned id. Used to verify updates aren't double
    // applied.
    $config.data.expectedCounters = {};

    // Because updates don't have a shard filter stage, a migration may fail if a
    // broadcast update is operating on orphans from a previous migration in the range being
    // migrated back in. The particular error code is replaced with a more generic one, so this is
    // identified by the failed migration's error message.
    $config.data.isMoveChunkErrorAcceptable = (err) => {
        return err.message &&
            (err.message.includes("CommandFailed") ||
             err.message.includes("Documents in target range may still be in use") ||
             // This error can occur when the test updates the shard key value of a document whose
             // chunk has been moved to another shard. Receiving a chunk only waits for documents
             // with shard key values in that range to have been cleaned up by the range deleter.
             // So, if the range deleter has not yet cleaned up that document when the chunk is
             // moved back to the original shard, the moveChunk may fail as a result of a duplicate
             // key error on the recipient.
             err.message.includes("Location51008"));
    };

    $config.data.runningWithStepdowns =
        TestData.runningWithConfigStepdowns || TestData.runningWithShardStepdowns;

    // These errors below may arrive due to expected scenarios that occur with concurrent
    // migrations and shard key updates. These include transient transaction errors (targeting
    // issues, lock timeouts, etc) and duplicate key errors that are triggered during normal
    // execution, likely due to orphan documents.
    //
    // Current server code does not retry on these errors, but the errors do not represent an
    // unrecoverable state. If an update fails in one of the above-described scenarios, we assert
    // that the document remains in the pre-updated state. After doing so, we may continue the
    // concurrency test.
    $config.data.isUpdateShardKeyErrorAcceptable = function isUpdateShardKeyAcceptable(
        errCode, errMsg, errorLabels) {
        if (!errMsg) {
            return false;
        }

        if (errorLabels && errorLabels.includes("TransientTransactionError")) {
            return true;
        }

        const failureInRetryableWriteToTxnConversionMsg =
            "Cannot retry a retryable write that has been converted";
        const duplicateKeyInChangeShardKeyMsg = "Failed to update document's shard key field";
        const otherErrorsInChangeShardKeyMsg = "was converted into a distributed transaction";

        if (errMsg.includes(failureInRetryableWriteToTxnConversionMsg) ||
            errMsg.includes(duplicateKeyInChangeShardKeyMsg)) {
            return true;
        }

        // Some return paths will strip out the TransientTransactionError label. We want to still
        // filter out those errors.
        let skippableErrors = [
            ErrorCodes.StaleConfig,
            ErrorCodes.WriteConflict,
            ErrorCodes.LockTimeout,
            ErrorCodes.PreparedTransactionInProgress,
            ErrorCodes.ShardInvalidatedForTargeting,
            ErrorCodes.NoSuchTransaction
        ];

        // If we're running in a stepdown suite, then attempting to update the shard key may
        // interact with stepdowns and transactions to cause the following errors. We only expect
        // these errors in stepdown suites and not in other suites, so we surface them to the test
        // runner in other scenarios.
        const stepdownErrors = [ErrorCodes.ConflictingOperationInProgress];

        if (this.runningWithStepdowns) {
            skippableErrors.push(...stepdownErrors);
        }

        // Failed in the document shard key path, but not with a duplicate key error
        if (errMsg.includes(otherErrorsInChangeShardKeyMsg)) {
            return skippableErrors.includes(errCode);
        }

        return false;
    };

    $config.data.calculateShardKeyUpdateValues = function calculateShardKeyUpdateValues(
        collection, collName, shardKeyField, moveAcrossChunks) {
        const idToUpdate = this.getIdForThread(collName);
        const randomDocToUpdate = collection.findOne({_id: idToUpdate});
        assertWhenOwnColl.neq(randomDocToUpdate, null);

        print("Updating the shard key field for this document: " + tojson(randomDocToUpdate));

        const currentShardKey = randomDocToUpdate[shardKeyField];

        const partitionSizeHalf = Math.floor(this.partitionSize / 2);
        const partitionMedian = partitionSizeHalf + this.partition.lower;

        let newShardKey = currentShardKey;
        while (newShardKey == currentShardKey) {
            // If moveAcrossChunks is true, move the randomly generated shardKey to the other
            // half of the partition, which will be on the other chunk owned by this thread.
            newShardKey = this.partition.lower + Math.floor(Math.random() * partitionSizeHalf);

            if (moveAcrossChunks || currentShardKey >= partitionMedian) {
                newShardKey += partitionSizeHalf;
            }
        }

        return {
            idToUpdate: idToUpdate,
            currentShardKey: currentShardKey,
            newShardKey: newShardKey,
            counterForId: this.expectedCounters[idToUpdate]
        };
    };

    $config.data.generateRandomUpdateStyle = function generateRandomUpdateStyle(
        currentId, newShardKey, counterForId) {
        const replacementStyle = (Math.floor(Math.random() * 2) == 0);

        if (replacementStyle) {
            return {_id: currentId, skey: newShardKey, counter: counterForId + 1};
        } else {
            // Op style
            return {$set: {skey: newShardKey}, $inc: {counter: 1}};
        }
    };

    $config.data.runInTransactionOrRetryableWrite = function runInTransactionOrRetryableWrite(
        functionToRun, wrapInTransaction) {
        if (wrapInTransaction) {
            withTxnAndAutoRetry(this.session, functionToRun);
        } else {
            functionToRun();
        }
    };

    $config.data.logTestIterationStart = function logTestIterationStart(commandName,
                                                                        wrapInTransaction,
                                                                        moveAcrossChunks,
                                                                        idToUpdate,
                                                                        currentShardKey,
                                                                        newShardKey,
                                                                        counterForId) {
        let logString = "Running " + commandName;
        logString += wrapInTransaction ? " inside a multi-statement transaction. "
                                       : " as a retryable write. ";
        logString += "The document will ";
        logString += moveAcrossChunks ? "move across chunks. " : "stay within the same chunk. \n";
        logString += "Original document values -- id: " + idToUpdate +
            ", shardKey: " + currentShardKey + ", counter: " + counterForId + "\n";
        logString += "Intended new document values -- shardKey: " + newShardKey +
            ", counter: " + (counterForId + 1);
        jsTestLog(logString);
    };

    $config.data.findAndLogUpdateResults = function findAndLogUpdateResults(
        collection, idToUpdate, currentShardKey, newShardKey) {
        let logString = "Going to print post-update state for id: " + idToUpdate +
            ", currentShardKey: " + currentShardKey + ", newShardKey: " + newShardKey + "\n";
        logString += "Find by old shard key (should be empty): " +
            tojson(collection.find({skey: currentShardKey}).toArray()) + "\n";
        logString += "Find by _id: " + tojson(collection.find({_id: idToUpdate}).toArray()) + "\n";
        logString +=
            "Find by new shard key: " + tojson(collection.find({skey: newShardKey}).toArray()) +
            "\n";

        jsTestLog(logString);
    };

    $config.data.findAndModifyShardKey = function findAndModifyShardKey(
        db, collName, {wrapInTransaction, moveAcrossChunks} = {}) {
        const collection = this.session.getDatabase(db.getName()).getCollection(collName);
        const shardKeyField = this.shardKeyField[collName];

        const {idToUpdate, currentShardKey, newShardKey, counterForId} =
            this.calculateShardKeyUpdateValues(
                collection, collName, shardKeyField, moveAcrossChunks);

        this.logTestIterationStart("findAndModify",
                                   wrapInTransaction,
                                   moveAcrossChunks,
                                   idToUpdate,
                                   currentShardKey,
                                   newShardKey,
                                   counterForId);

        let performFindAndModify = () => {
            try {
                const modifiedDoc = collection.findAndModify({
                    query: {_id: idToUpdate, skey: currentShardKey},
                    update: this.generateRandomUpdateStyle(idToUpdate, newShardKey, counterForId),
                    upsert: true,
                    new: true
                });
                assertWhenOwnColl.neq(modifiedDoc, null);
                assertWhenOwnColl.eq(collection.findOne({_id: idToUpdate, skey: newShardKey}),
                                     modifiedDoc);

                this.expectedCounters[idToUpdate] = counterForId + 1;
            } catch (e) {
                const msg = e.errmsg ? e.errmsg : e.message;
                if (this.isUpdateShardKeyErrorAcceptable(e.code, msg, e.errorLabels)) {
                    print("Ignoring acceptable updateShardKey error attempting to update the" +
                          "document with _id: " + idToUpdate + " and shardKey: " + currentShardKey +
                          ": " + e);
                    assertWhenOwnColl.neq(
                        collection.findOne({_id: idToUpdate, skey: currentShardKey}), null);
                    assertWhenOwnColl.eq(
                        collection.findOne(
                            {_id: idToUpdate, skey: newShardKey, counter: counterForId}),
                        null);
                    return;
                }
                throw e;
            }
        };

        this.runInTransactionOrRetryableWrite(performFindAndModify, wrapInTransaction);

        this.findAndLogUpdateResults(collection, idToUpdate, currentShardKey, newShardKey);
    };

    $config.data.updateShardKey = function updateShardKey(
        db, collName, {moveAcrossChunks, wrapInTransaction} = {}) {
        const collection = this.session.getDatabase(db.getName()).getCollection(collName);
        const shardKeyField = this.shardKeyField[collName];

        const {idToUpdate, currentShardKey, newShardKey, counterForId} =
            this.calculateShardKeyUpdateValues(
                collection, collName, shardKeyField, moveAcrossChunks);

        this.logTestIterationStart("update",
                                   wrapInTransaction,
                                   moveAcrossChunks,
                                   idToUpdate,
                                   currentShardKey,
                                   newShardKey,
                                   counterForId);

        let performUpdate = () => {
            const updateResult = collection.update(
                {_id: idToUpdate, skey: currentShardKey},
                this.generateRandomUpdateStyle(idToUpdate, newShardKey, counterForId),
                {multi: false});
            try {
                assertWhenOwnColl.commandWorked(updateResult);
                this.expectedCounters[idToUpdate] = counterForId + 1;
            } catch (e) {
                const err = updateResult instanceof WriteResult ? updateResult.getWriteError()
                                                                : updateResult;

                if (this.isUpdateShardKeyErrorAcceptable(err.code, err.errmsg, err.errorLabels)) {
                    print("Ignoring acceptable updateShardKey error attempting to update the" +
                          "document with _id: " + idToUpdate + " and shardKey: " + currentShardKey +
                          ": " + tojson(updateResult));
                    assertWhenOwnColl.neq(
                        collection.findOne({_id: idToUpdate, skey: currentShardKey}), null);
                    assertWhenOwnColl.eq(
                        collection.findOne(
                            {_id: idToUpdate, skey: newShardKey, counter: counterForId}),
                        null);
                    return;
                }
                throw e;
            }
        };

        this.runInTransactionOrRetryableWrite(performUpdate, wrapInTransaction);

        this.findAndLogUpdateResults(collection, idToUpdate, currentShardKey, newShardKey);
    };

    /**
     * The following states are enumerations of these options:
     * 1. Run a findAndModify or update command.
     * 2. Run under a multi-statement transaction or a retryable write.
     * 3. Target the update to move chunks or remain on the same chunk.
     */
    $config.states.findAndModifyWithRetryableWriteAcrossChunks =
        function findAndModifyWithRetryableWriteAcrossChunks(db, collName, connCache) {
        this.findAndModifyShardKey(
            db, collName, {wrapInTransaction: false, moveAcrossChunks: true});
    };

    $config.states.findAndModifyWithRetryableWriteWithinChunk =
        function findAndModifyWithRetryableWriteWithinChunk(db, collName, connCache) {
        this.findAndModifyShardKey(
            db, collName, {wrapInTransaction: false, moveAcrossChunks: false});
    };

    $config.states.findAndModifyWithTransactionAcrossChunks =
        function findAndModifyWithTransactionAcrossChunks(db, collName, connCache) {
        this.findAndModifyShardKey(db, collName, {wrapInTransaction: true, moveAcrossChunks: true});
    };

    $config.states.findAndModifyWithTransactionWithinChunk =
        function findAndModifyWithTransactionWithinChunk(db, collName, connCache) {
        this.findAndModifyShardKey(
            db, collName, {wrapInTransaction: true, moveAcrossChunks: false});
    };

    $config.states.updateWithRetryableWriteAcrossChunks =
        function updateWithRetryableWriteAcrossChunks(db, collName, connCache) {
        this.updateShardKey(db, collName, {wrapInTransaction: false, moveAcrossChunks: true});
    };

    $config.states.updateWithRetryableWriteWithinChunk =
        function updateWithRetryableWriteWithinChunk(db, collName, connCache) {
        this.updateShardKey(db, collName, {wrapInTransaction: false, moveAcrossChunks: false});
    };

    $config.states.updateWithTransactionAcrossChunks = function updateWithTransactionAcrossChunks(
        db, collName, connCache) {
        this.updateShardKey(db, collName, {wrapInTransaction: true, moveAcrossChunks: true});
    };

    $config.states.updateWithTransactionWithinChunk = function updateWithTransactionWithinChunk(
        db, collName, connCache) {
        this.updateShardKey(db, collName, {wrapInTransaction: true, moveAcrossChunks: false});
    };

    /**
     * Sets up the base workload, starts a session, and gives each document assigned to this thread
     * a counter value that is tracked in-memory.
     */
    $config.states.init = function init(db, collName, connCache) {
        $super.states.init.apply(this, arguments);

        this.session = db.getMongo().startSession({causalConsistency: false, retryWrites: true});

        // Assign a default counter value to each document owned by this thread.
        db[collName].find({tid: this.tid}).forEach(doc => {
            this.expectedCounters[doc._id] = 0;
            assert.commandWorked(db[collName].update({_id: doc._id}, {$set: {counter: 0}}));
        });
    };

    /**
     * Sets up the collection so each thread's partition is a single chunk, with partitionSize
     * documents within it, randomly assigning each document to a thread, ensuring at least one
     * document is given to each one.
     */
    $config.setup = function setup(db, collName, cluster) {
        db.setLogLevel(3);
        const ns = db[collName].getFullName();

        for (let tid = 0; tid < this.threadCount; ++tid) {
            // Find the thread's partition.
            const partition = this.makePartition(ns, tid, this.partitionSize);
            const medianIdForThread = partition.lower + Math.floor(this.partitionSize / 2);

            let bulk = db[collName].initializeUnorderedBulkOp();
            for (let i = partition.lower; i < medianIdForThread; ++i) {
                bulk.insert({_id: i, skey: i, tid: tid});
            }

            assertAlways.commandWorked(bulk.execute());

            // Create a chunk with boundaries matching the partition's. The low chunk's lower bound
            // is minKey, so a split is not necessary.
            if (!partition.isLowChunk) {
                assertAlways.commandWorked(
                    db.adminCommand({split: ns, middle: {skey: partition.lower}}));
            }
            assertAlways.commandWorked(
                db.adminCommand({split: ns, middle: {skey: medianIdForThread}}));
        }
        db.printShardingStatus();
    };

    /**
     * Asserts all documents assigned to this thread match their expected values.
     */
    $config.states.verifyDocuments = function verifyDocuments(db, collName, connCache) {
        const docs = db[collName].find({tid: this.tid}).toArray();
        docs.forEach(doc => {
            const expectedCounter = this.expectedCounters[doc._id];
            assertWhenOwnColl.eq(expectedCounter, doc.counter, () => {
                return 'unexpected counter value, doc: ' + tojson(doc);
            });
        });
    };

    $config.transitions = {
        init: {
            moveChunk: 0.2,
            findAndModifyWithRetryableWriteAcrossChunks: 0.1,
            findAndModifyWithRetryableWriteWithinChunk: 0.1,
            findAndModifyWithTransactionAcrossChunks: 0.1,
            findAndModifyWithTransactionWithinChunk: 0.1,
            updateWithRetryableWriteAcrossChunks: 0.1,
            updateWithRetryableWriteWithinChunk: 0.1,
            updateWithTransactionAcrossChunks: 0.1,
            updateWithTransactionWithinChunk: 0.1
        },
        moveChunk: {
            moveChunk: 0.2,
            findAndModifyWithRetryableWriteAcrossChunks: 0.075,
            findAndModifyWithRetryableWriteWithinChunk: 0.075,
            findAndModifyWithTransactionAcrossChunks: 0.075,
            findAndModifyWithTransactionWithinChunk: 0.075,
            updateWithRetryableWriteAcrossChunks: 0.075,
            updateWithRetryableWriteWithinChunk: 0.075,
            updateWithTransactionAcrossChunks: 0.075,
            updateWithTransactionWithinChunk: 0.075,
            verifyDocuments: 0.2
        },
        findAndModifyWithRetryableWriteAcrossChunks: {
            moveChunk: 0.2,
            findAndModifyWithRetryableWriteAcrossChunks: 0.075,
            findAndModifyWithRetryableWriteWithinChunk: 0.075,
            findAndModifyWithTransactionAcrossChunks: 0.075,
            findAndModifyWithTransactionWithinChunk: 0.075,
            updateWithRetryableWriteAcrossChunks: 0.075,
            updateWithRetryableWriteWithinChunk: 0.075,
            updateWithTransactionAcrossChunks: 0.075,
            updateWithTransactionWithinChunk: 0.075,
            verifyDocuments: 0.2
        },
        findAndModifyWithRetryableWriteWithinChunk: {
            moveChunk: 0.2,
            findAndModifyWithRetryableWriteAcrossChunks: 0.075,
            findAndModifyWithRetryableWriteWithinChunk: 0.075,
            findAndModifyWithTransactionAcrossChunks: 0.075,
            findAndModifyWithTransactionWithinChunk: 0.075,
            updateWithRetryableWriteAcrossChunks: 0.075,
            updateWithRetryableWriteWithinChunk: 0.075,
            updateWithTransactionAcrossChunks: 0.075,
            updateWithTransactionWithinChunk: 0.075,
            verifyDocuments: 0.2
        },
        findAndModifyWithTransactionAcrossChunks: {
            moveChunk: 0.2,
            findAndModifyWithRetryableWriteAcrossChunks: 0.075,
            findAndModifyWithRetryableWriteWithinChunk: 0.075,
            findAndModifyWithTransactionAcrossChunks: 0.075,
            findAndModifyWithTransactionWithinChunk: 0.075,
            updateWithRetryableWriteAcrossChunks: 0.075,
            updateWithRetryableWriteWithinChunk: 0.075,
            updateWithTransactionAcrossChunks: 0.075,
            updateWithTransactionWithinChunk: 0.075,
            verifyDocuments: 0.2
        },
        findAndModifyWithTransactionWithinChunk: {
            moveChunk: 0.2,
            findAndModifyWithRetryableWriteAcrossChunks: 0.075,
            findAndModifyWithRetryableWriteWithinChunk: 0.075,
            findAndModifyWithTransactionAcrossChunks: 0.075,
            findAndModifyWithTransactionWithinChunk: 0.075,
            updateWithRetryableWriteAcrossChunks: 0.075,
            updateWithRetryableWriteWithinChunk: 0.075,
            updateWithTransactionAcrossChunks: 0.075,
            updateWithTransactionWithinChunk: 0.075,
            verifyDocuments: 0.2
        },
        updateWithRetryableWriteAcrossChunks: {
            moveChunk: 0.2,
            findAndModifyWithRetryableWriteAcrossChunks: 0.075,
            findAndModifyWithRetryableWriteWithinChunk: 0.075,
            findAndModifyWithTransactionAcrossChunks: 0.075,
            findAndModifyWithTransactionWithinChunk: 0.075,
            updateWithRetryableWriteAcrossChunks: 0.075,
            updateWithRetryableWriteWithinChunk: 0.075,
            updateWithTransactionAcrossChunks: 0.075,
            updateWithTransactionWithinChunk: 0.075,
            verifyDocuments: 0.2
        },
        updateWithRetryableWriteWithinChunk: {
            moveChunk: 0.2,
            findAndModifyWithRetryableWriteAcrossChunks: 0.075,
            findAndModifyWithRetryableWriteWithinChunk: 0.075,
            findAndModifyWithTransactionAcrossChunks: 0.075,
            findAndModifyWithTransactionWithinChunk: 0.075,
            updateWithRetryableWriteAcrossChunks: 0.075,
            updateWithRetryableWriteWithinChunk: 0.075,
            updateWithTransactionAcrossChunks: 0.075,
            updateWithTransactionWithinChunk: 0.075,
            verifyDocuments: 0.2
        },
        updateWithTransactionAcrossChunks: {
            moveChunk: 0.2,
            findAndModifyWithRetryableWriteAcrossChunks: 0.075,
            findAndModifyWithRetryableWriteWithinChunk: 0.075,
            findAndModifyWithTransactionAcrossChunks: 0.075,
            findAndModifyWithTransactionWithinChunk: 0.075,
            updateWithRetryableWriteAcrossChunks: 0.075,
            updateWithRetryableWriteWithinChunk: 0.075,
            updateWithTransactionAcrossChunks: 0.075,
            updateWithTransactionWithinChunk: 0.075,
            verifyDocuments: 0.2
        },
        updateWithTransactionWithinChunk: {
            moveChunk: 0.2,
            findAndModifyWithRetryableWriteAcrossChunks: 0.075,
            findAndModifyWithRetryableWriteWithinChunk: 0.075,
            findAndModifyWithTransactionAcrossChunks: 0.075,
            findAndModifyWithTransactionWithinChunk: 0.075,
            updateWithRetryableWriteAcrossChunks: 0.075,
            updateWithRetryableWriteWithinChunk: 0.075,
            updateWithTransactionAcrossChunks: 0.075,
            updateWithTransactionWithinChunk: 0.075,
            verifyDocuments: 0.2
        },
        verifyDocuments: {
            moveChunk: 0.2,
            findAndModifyWithRetryableWriteAcrossChunks: 0.075,
            findAndModifyWithRetryableWriteWithinChunk: 0.075,
            findAndModifyWithTransactionAcrossChunks: 0.075,
            findAndModifyWithTransactionWithinChunk: 0.075,
            updateWithRetryableWriteAcrossChunks: 0.075,
            updateWithRetryableWriteWithinChunk: 0.075,
            updateWithTransactionAcrossChunks: 0.075,
            updateWithTransactionWithinChunk: 0.075,
            verifyDocuments: 0.2
        },
    };

    return $config;
});