/**
 * kill_rooted_or.js
 *
 * Queries using a rooted $or predicate to cause plan selection to use the subplanner. Tests that
 * the subplanner correctly halts plan execution when the collection is dropped or a candidate index
 * is dropped.
 *
 * This workload was designed to reproduce SERVER-24761.
 * @tags: [
 *   requires_getmore
 * ]
 */
import {assertWorkedOrFailedHandleTxnErrors} from "jstests/concurrency/fsm_workload_helpers/assert_handle_fail_in_transaction.js";

export const $config = (function () {
    // Use the workload name as the collection name, since the workload name is assumed to be
    // unique. Note that we choose our own collection name instead of using the collection provided
    // by the concurrency framework, because this workload drops its collection.
    let uniqueCollectionName = "kill_rooted_or";

    let data = {
        collName: uniqueCollectionName,
        indexSpecs: [{a: 1}, {a: 1, c: 1}, {b: 1}, {b: 1, c: 1}],
        numDocs: 200,
    };

    let states = {
        query: function query(db, collNameUnused) {
            let cursor = db[this.collName].find({$or: [{a: 0}, {b: 0}]});
            try {
                // We don't know exactly how many documents will be in the collection at the time of
                // the query, so we can't verify this value.
                cursor.itcount();
            } catch (e) {
                // We expect to see errors caused by the plan executor being killed, because of the
                // collection getting dropped on another thread.
                const kAllowedErrorCodes = [ErrorCodes.QueryPlanKilled, ErrorCodes.NamespaceNotFound];
                if (!kAllowedErrorCodes.includes(e.code)) {
                    throw e;
                }
            }
        },

        dropCollection: function dropCollection(db, collNameUnused) {
            db[this.collName].drop();

            // Restore the collection.
            populateIndexes(db[this.collName], this.indexSpecs);
            populateCollection(db[this.collName], this.numDocs);
        },

        dropIndex: function dropIndex(db, collNameUnused) {
            let indexSpec = this.indexSpecs[Random.randInt(this.indexSpecs.length)];

            // We don't assert that the command succeeded when dropping an index because it's
            // possible another thread has already dropped this index.
            db[this.collName].dropIndex(indexSpec);

            // Recreate the index that was dropped. (See populateIndexes() for why we ignore the
            // CannotImplicitlyCreateCollection error.)
            let res = db[this.collName].createIndex(indexSpec);
            assertWorkedOrFailedHandleTxnErrors(
                res,
                [
                    ErrorCodes.CannotImplicitlyCreateCollection,
                    ErrorCodes.IndexBuildAborted,
                    ErrorCodes.IndexBuildAlreadyInProgress,
                    ErrorCodes.NoMatchingDocument,
                    ErrorCodes.StaleConfig,
                ],
                [
                    ErrorCodes.CannotImplicitlyCreateCollection,
                    ErrorCodes.IndexBuildAborted,
                    ErrorCodes.NoMatchingDocument,
                    ErrorCodes.StaleConfig,
                ],
            );
        },
    };

    let transitions = {
        query: {query: 0.8, dropCollection: 0.1, dropIndex: 0.1},
        dropCollection: {query: 1},
        dropIndex: {query: 1},
    };

    function populateIndexes(coll, indexSpecs) {
        indexSpecs.forEach((indexSpec) => {
            // In sharded configurations, there's a limit to how many times mongos can retry an
            // operation that fails because it wants to implicitly create a collection that is
            // concurrently dropped. Normally, that's fine, but if some jerk keeps dropping our
            // collection (as in the 'dropCollection' state of this test), then we run out of
            // retries and get a CannotImplicitlyCreateCollection error once in a while, which we
            // have to ignore.
            assertWorkedOrFailedHandleTxnErrors(
                coll.createIndex(indexSpec),
                [
                    ErrorCodes.CannotImplicitlyCreateCollection,
                    ErrorCodes.IndexBuildAborted,
                    ErrorCodes.IndexBuildAlreadyInProgress,
                    ErrorCodes.NoMatchingDocument,
                    ErrorCodes.StaleConfig,
                ],
                [
                    ErrorCodes.CannotImplicitlyCreateCollection,
                    ErrorCodes.IndexBuildAborted,
                    ErrorCodes.NoMatchingDocument,
                    ErrorCodes.StaleConfig,
                ],
            );
        });
    }

    function populateCollection(coll, numDocs) {
        // See populateIndexes() for why we ignore CannotImplicitlyCreateCollection errors.
        // Similarly, this bulk insert can also give up with a NoProgressMade error after repeated
        // attempts in the sharded causal consistency configuration. We also ignore that error.
        const bulkInsertResult = coll.insert(Array(numDocs).fill({a: 0, b: 0, c: 0}));
        assert(!bulkInsertResult.hasWriteConcernError(), bulkInsertResult);
        bulkInsertResult.getWriteErrors().forEach((err) => {
            assert.contains(err.code, [
                ErrorCodes.CannotImplicitlyCreateCollection,
                ErrorCodes.NoProgressMade,
                ErrorCodes.StaleConfig,
            ]);
        }, bulkInsertResult);
    }

    function setup(db, collNameUnused, cluster) {
        populateIndexes(db[this.collName], this.indexSpecs);
        populateCollection(db[this.collName], this.numDocs);
    }

    return {
        threadCount: 10,
        iterations: 50,
        data: data,
        states: states,
        startState: "query",
        transitions: transitions,
        setup: setup,
    };
})();
