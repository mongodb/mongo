/**
 * long_secondary_reads.js
 *
 * Read a large number of documents from a secondary node. The test expects to read the same number
 * of documents that were inserted without getting unexpected errors. The test focuses on migrations
 * and range deletions that occur during long-running queries. The more 'getMore' commands a query
 * has, the more yield-and-restore events will occur, increasing the likelihood that the query will
 * encounter a range deletion, possibly in combination with various hooks and other factors.
 *
 * @tags: [
 *   requires_getmore,
 *   uses_getmore_outside_of_transaction,
 *   # Tests which expect commands to fail and catch the error can cause transactions to abort and
 *   # retry indefinitely.
 *   catches_command_failures,
 *   requires_fcv_82
 * ]
 */

import {arrayDiff, orderedArrayEq} from "jstests/aggregation/extras/utils.js";
import {uniformDistTransitions} from "jstests/concurrency/fsm_workload_helpers/state_transition_utils.js";

export const $config = (function () {
    // Use the workload name as the collection name.
    const collectionName = jsTestName();

    function getReadConcernLevel() {
        const readConcernLevels = ["local", "majority"];
        const readConcernLevel = readConcernLevels[Random.randInt(readConcernLevels.length)];
        jsTestLog("Testing find with readConcern " + readConcernLevel);
        return readConcernLevel;
    }

    var states = {
        init: function (db, unusedCollName) {
            this.session = db.getMongo().startSession({causalConsistency: true});
            this.sessionDb = this.session.getDatabase(db.getName());
        },
        readFromSecondaries: function (unusedDB, unusedCollName) {
            // The query can be retried on a RetryableError and the following additional errors:
            // * The ErrorCodes.QueryPlanKilled error can occur when a query is terminated on a
            // secondary during a range deletion.
            // * The ErrorCodes.CursorNotFound error can occur during query execution in test
            // suites, typically happening when, for example, the getMore command cannot be
            // continued after simulating a crash.
            retryOnRetryableError(
                () => {
                    // WARNING: Avoid adding the sort option to the find command in order to prevent
                    // exhausting the cursor on the first call.
                    const arr = this.sessionDb[this.collName]
                        .find()
                        .readPref("secondary")
                        .readConcern(getReadConcernLevel())
                        .toArray();
                    assert.gt(arr.length, 0, "Failed to retrieve documents");
                    arr.sort((d1, d2) => d1._id - d2._id);
                    assert(orderedArrayEq(this.expectedDocuments, arr), () => arrayDiff(this.expectedDocuments, arr));
                },
                100 /* numRetries */,
                0 /* sleepMs */,
                [ErrorCodes.QueryPlanKilled, ErrorCodes.CursorNotFound],
            );
        },
        cleanup: function (unusedDB, unusedCollName) {
            this.session.endSession();
        },
    };

    var setup = function setup(db, unusedCollName, cluster) {
        this.expectedDocuments = Array.from({length: this.nDocumentsToInsert}).map((_, i) => ({_id: i, x: i}));
        assert.commandWorked(db[this.collName].insertMany(this.expectedDocuments));
        if (cluster.isReplication()) {
            cluster.awaitReplication();
        }
    };

    return {
        threadCount: 10,
        iterations: 10,
        startState: "init",
        states: states,
        data: {nDocumentsToInsert: 2000, collName: collectionName},
        transitions: uniformDistTransitions(states),
        setup: setup,
    };
})();
