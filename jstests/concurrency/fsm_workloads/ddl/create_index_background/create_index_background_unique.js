/**
 * create_index_background_unique.js
 *
 * Creates multiple unique background indexes in parallel.
 *
 * Marked as 'requires_persistence' to prevent the test from running on 'inMemory' variant, because
 * the test generates a large oplog and 'inMemory' instances have limited resources to accommodate
 * all nodes in the replica set (which all run in the same instance), so it may fail with the OOM
 * error.
 * @tags: [
 *     creates_background_indexes,
 *     requires_persistence,
 *     assumes_balancer_off,
 * ]
 */
import {assertWorkedOrFailedHandleTxnErrors} from "jstests/concurrency/fsm_workload_helpers/assert_handle_fail_in_transaction.js";

export const $config = (function () {
    let data = {
        prefix: "create_index_background_unique_",
        numDocsToLoad: 5000,
        iterationCount: 0,
        getCollectionNameForThread: function (threadId) {
            return this.prefix + threadId.toString();
        },
        // Allows tests that inherit from this one to specify options other than the default.
        getCollectionOptions: function () {
            return {};
        },
        buildvariableSizedDoc: function (uniquePrefix) {
            const indexedVal = uniquePrefix + Array(Random.randInt(1000)).toString();
            const doc = {x: indexedVal};
            return doc;
        },
    };

    let states = (function () {
        function buildIndex(db, collName) {
            this.iterationCount++;

            const res = db.runCommand({
                createIndexes: this.getCollectionNameForThread(this.tid),
                indexes: [{key: {x: 1}, name: "x_1", unique: true}],
            });
            // Multi-statement Transactions can fail with SnapshotUnavailable if there are
            // pending catalog changes as of the transaction start (see SERVER-43018).
            assertWorkedOrFailedHandleTxnErrors(
                res,
                [
                    ErrorCodes.IndexBuildAborted,
                    ErrorCodes.IndexBuildAlreadyInProgress,
                    ErrorCodes.SnapshotUnavailable,
                    ErrorCodes.SnapshotTooOld,
                    ErrorCodes.NoMatchingDocument,
                    ErrorCodes.NotWritablePrimary,
                    ErrorCodes.FailedToSatisfyReadPreference,
                ],
                [ErrorCodes.IndexBuildAborted, ErrorCodes.NoMatchingDocument, ErrorCodes.NotWritablePrimary],
            );
        }

        function dropIndex(db, collName) {
            this.iterationCount++;

            // In the case that we have an even # of iterations, we skip the final drop so that
            // validation can be performed on the indexes created.
            if (this.iterationCount === this.iterations) {
                return;
            }

            assert.commandWorked(db.runCommand({dropIndexes: this.getCollectionNameForThread(this.tid), index: "x_1"}));
        }

        return {
            buildIndex: buildIndex,
            dropIndex: dropIndex,
        };
    })();

    let transitions = {
        buildIndex: {dropIndex: 1.0},
        dropIndex: {buildIndex: 1.0},
    };

    function setup(db, collName, cluster) {
        for (let j = 0; j < this.threadCount; ++j) {
            const collectionName = this.getCollectionNameForThread(j);
            assert.commandWorked(db.createCollection(collectionName, this.getCollectionOptions()));
            let bulk = db[collectionName].initializeUnorderedBulkOp();

            // Preload documents for each thread's collection. This ensures that the index build and
            // drop have meaningful work to do.
            for (let i = 0; i < this.numDocsToLoad; ++i) {
                const uniqueValuePrefix = i.toString() + "_";
                bulk.insert(this.buildvariableSizedDoc(uniqueValuePrefix));
            }
            assert.commandWorked(bulk.execute());
            assert.eq(this.numDocsToLoad, db[collectionName].find({}).itcount());
        }
    }

    return {
        threadCount: 10,
        iterations: 11,
        data: data,
        states: states,
        startState: "buildIndex",
        transitions: transitions,
        setup: setup,
    };
})();
