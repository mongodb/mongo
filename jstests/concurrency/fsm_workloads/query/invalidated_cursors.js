/**
 * invalidated_cursors.js
 *
 * This workload was designed to stress creating, pinning, and invalidating cursors through the
 * cursor manager. Threads perform find, getMore and explain commands while the database,
 * collection, or an index is dropped.
 *
 * @tags: [
 *   uses_curop_agg_stage,
 *   state_functions_share_cursor,
 *   assumes_balancer_off,
 *   requires_getmore,
 * ]
 */

import {interruptedQueryErrors} from "jstests/concurrency/fsm_libs/assert.js";
import {assertWorkedOrFailedHandleTxnErrors} from "jstests/concurrency/fsm_workload_helpers/assert_handle_fail_in_transaction.js";
import {isMongos} from "jstests/concurrency/fsm_workload_helpers/server_types.js";

export const $config = (function () {
    let data = {
        chooseRandomlyFrom: function chooseRandomlyFrom(arr) {
            if (!Array.isArray(arr)) {
                throw new Error("Expected array for first argument, but got: " + tojson(arr));
            }
            return arr[Random.randInt(arr.length)];
        },

        involvedCollections: ["coll0", "coll1", "coll2"],
        indexSpecs: [{a: 1, b: 1}, {c: 1}],

        numDocs: 100,
        batchSize: 2,
        isCreateIndexRequested: false,
        isCreatedSucceedAtLeastOnce: false,
        createIndexAndAssert: function (db, collName, indexSpecs) {
            const errorCodesTxn = [
                ErrorCodes.DatabaseDropPending,
                ErrorCodes.IndexBuildAborted,
                ErrorCodes.IndexBuildAlreadyInProgress,
                ErrorCodes.NoMatchingDocument,
            ];
            const errorCodesNonTxn = [
                ErrorCodes.DatabaseDropPending,
                ErrorCodes.IndexBuildAborted,
                ErrorCodes.NoMatchingDocument,
            ];
            // TODO(SERVER-18047): Unify error codes once an explain against a non-existent
            // database fails in an unsharded environment.
            if (isMongos(db) || TestData.testingReplicaSetEndpoint) {
                errorCodesNonTxn.push(ErrorCodes.NamespaceNotFound);
                errorCodesNonTxn.push(ErrorCodes.CannotImplicitlyCreateCollection);
                errorCodesNonTxn.push(ErrorCodes.StaleConfig);
                errorCodesTxn.push(ErrorCodes.CannotImplicitlyCreateCollection);
            }
            this.isCreateIndexRequested = true;
            indexSpecs.forEach((indexSpec) => {
                const res = db[collName].createIndex(indexSpec);
                assertWorkedOrFailedHandleTxnErrors(res, errorCodesTxn, errorCodesNonTxn);
                if (res.ok) {
                    this.isCreatedSucceedAtLeastOnce = true;
                }
            });
        },
        /**
         * Inserts 'this.numDocs' new documents into the specified collection and ensures that the
         * indexes 'this.indexSpecs' exist on the collection. Note that means it is safe for
         * multiple threads to perform this function simultaneously.
         */
        populateDataAndIndexes: function populateDataAndIndexes(db, collName) {
            try {
                let bulk = db[collName].initializeUnorderedBulkOp();
                for (let i = 0; i < this.numDocs; ++i) {
                    bulk.insert({});
                }
                let res = bulk.execute();
                assert.commandWorked(res);
                assert.eq(this.numDocs, res.nInserted, tojson(res));
            } catch (ex) {
                assert.eq(true, ex instanceof BulkWriteError);
                assert.writeErrorWithCode(ex, ErrorCodes.DatabaseDropPending);
            }

            this.createIndexAndAssert(db, collName, this.indexSpecs);
        },

        /**
         * Calls 'killFn' on a random getMore that's currently running.
         */
        killRandomGetMore: function killRandomGetMore(someDB, killFn) {
            const admin = someDB.getSiblingDB("admin");
            const getMores = admin
                .aggregate([
                    // idleConnections true so we can also kill cursors which are
                    // not currently active.
                    // localOps true so that currentOp reports the mongos
                    // operations when run on a sharded cluster, instead of the
                    // shard's operations.
                    {$currentOp: {idleConnections: true, localOps: true}},
                    // We only about getMores.
                    {$match: {"command.getMore": {$exists: true}}},
                    // Only find getMores running on the database for this test.
                    {$match: {"ns": {$regex: this.uniqueDBName + "\."}}},
                ])
                .toArray();

            if (getMores.length === 0) {
                return;
            }

            const toKill = this.chooseRandomlyFrom(getMores);
            return killFn(toKill);
        },
    };

    let states = {
        /**
         * This is a no-op, used only as a transition state.
         */
        init: function init(db, collName) {},

        /**
         * Runs a query on the collection with a small enough batchSize to leave the cursor open.
         * If the command was successful, stores the resulting cursor in 'this.cursor'.
         */
        query: function query(unusedDB, unusedCollName) {
            let myDB = unusedDB.getSiblingDB(this.uniqueDBName);
            let res = myDB.runCommand({
                find: this.chooseRandomlyFrom(this.involvedCollections),
                filter: {},
                batchSize: this.batchSize,
            });

            if (res.ok) {
                this.cursor = new DBCommandCursor(myDB, res, this.batchSize);
            }
        },

        /**
         * Explains a find on a collection.
         */
        explain: function explain(unusedDB, unusedCollName) {
            let myDB = unusedDB.getSiblingDB(this.uniqueDBName);
            let res = myDB.runCommand({
                explain: {find: this.chooseRandomlyFrom(this.involvedCollections), filter: {}},
                verbosity: "executionStats",
            });
            assert.commandWorked(res);
        },

        /**
         * This is just a transition state that serves as a placeholder to delegate to one of the
         * specific kill types like 'killOp' or 'killCursors'.
         */
        kill: function kill(unusedDB, unusedCollName) {},

        /**
         * Choose a random cursor that's open and kill it.
         */
        killCursor: function killCursor(unusedDB, unusedCollName) {
            const myDB = unusedDB.getSiblingDB(this.uniqueDBName);

            // Not checking the return value, since the cursor may be closed on its own
            // before this has a chance to run.
            this.killRandomGetMore(myDB, function (toKill) {
                const res = myDB.runCommand({
                    killCursors: toKill.command.collection,
                    cursors: [toKill.command.getMore],
                });
                assert.commandWorked(res);
            });
        },

        killOp: function killOp(unusedDB, unusedCollName) {
            const myDB = unusedDB.getSiblingDB(this.uniqueDBName);
            // Not checking return value since the operation may end on its own before we have
            // a chance to kill it.
            this.killRandomGetMore(myDB, function (toKill) {
                assert.commandWorked(myDB.killOp(toKill.opid));
            });
        },

        /**
         * Requests enough results from 'this.cursor' to ensure that another batch is needed, and
         * thus ensures that a getMore request is sent for 'this.cursor'.
         */
        getMore: function getMore(unusedDB, unusedCollName) {
            if (!this.hasOwnProperty("cursor")) {
                return;
            }

            for (let i = 0; i <= this.batchSize; ++i) {
                try {
                    if (!this.cursor.hasNext()) {
                        break;
                    }
                    this.cursor.next();
                } catch (e) {
                    // The getMore request can fail if the database, a collection, or an index was
                    // dropped. It can also fail if another thread kills it through killCursor or
                    // killOp.
                    const expectedErrors = [
                        ErrorCodes.NamespaceNotFound,
                        ErrorCodes.OperationFailed,
                        ...interruptedQueryErrors,
                    ];
                    if (!expectedErrors.includes(e.code)) {
                        throw e;
                    }
                }
            }
        },

        /**
         * Drops the database being used by this workload and then re-creates each of
         * 'this.involvedCollections' by repopulating them with data and indexes.
         */
        dropDatabase: function dropDatabase(unusedDB, unusedCollName) {
            let myDB = unusedDB.getSiblingDB(this.uniqueDBName);
            myDB.dropDatabase();

            // Re-create all of the collections and indexes that were dropped.
            this.involvedCollections.forEach((collName) => {
                this.populateDataAndIndexes(myDB, collName);
            });
        },

        /**
         * Randomly selects a collection from 'this.involvedCollections' and drops it. The
         * collection is then re-created with data and indexes.
         */
        dropCollection: function dropCollection(unusedDB, unusedCollName) {
            let myDB = unusedDB.getSiblingDB(this.uniqueDBName);
            let targetColl = this.chooseRandomlyFrom(this.involvedCollections);

            myDB[targetColl].drop();

            // Re-create the collection that was dropped.
            this.populateDataAndIndexes(myDB, targetColl);
        },

        /**
         * Randomly selects a collection from 'this.involvedCollections' and an index from
         * 'this.indexSpecs' and drops that particular index from the collection. The index is then
         * re-created.
         */
        dropIndex: function dropIndex(unusedDB, unusedCollName) {
            let myDB = unusedDB.getSiblingDB(this.uniqueDBName);
            let targetColl = this.chooseRandomlyFrom(this.involvedCollections);
            let indexSpec = this.chooseRandomlyFrom(this.indexSpecs);

            // We don't assert that the command succeeded when dropping an index because it's
            // possible another thread has already dropped this index.
            myDB[targetColl].dropIndex(indexSpec);

            this.createIndexAndAssert(myDB, targetColl, [indexSpec]);
        },
    };

    let transitions = {
        init: {
            query: 0.6,
            explain: 0.1,
            dropDatabase: 0.1,
            dropCollection: 0.1,
            dropIndex: 0.1,
        },

        query: {kill: 0.1, getMore: 0.9},
        explain: {explain: 0.1, init: 0.9},
        kill: {killOp: 0.5, killCursor: 0.5},
        killOp: {init: 1},
        killCursor: {init: 1},
        getMore: {kill: 0.2, getMore: 0.6, init: 0.2},
        dropDatabase: {init: 1},
        dropCollection: {init: 1},
        dropIndex: {init: 1},
    };

    function setup(unusedDB, unusedCollName, cluster) {
        // Use the workload name as part of the database name, since the workload name is assumed to
        // be unique.
        this.uniqueDBName = unusedDB.getName() + "invalidated_cursors";

        let myDB = unusedDB.getSiblingDB(this.uniqueDBName);
        this.involvedCollections.forEach((collName) => {
            this.populateDataAndIndexes(myDB, collName);
            assert.eq(this.numDocs, myDB[collName].find({}).itcount());
        });
    }

    function teardown(db, collName, cluster) {
        // Assert the createIndex succeeded at least once if requested. Considering the level of
        // concurrency of this test, it's highly unluckely all the createIndex will fail. The test
        // swallows many rare transient errors. The approach could potentially mask an underling
        // issue if those errors happen consistently and no creation go through.
        assert.eq(this.isCreateIndexRequested, this.isCreatedSucceedAtLeastOnce);
    }

    return {
        threadCount: 10,
        iterations: 200,
        states: states,
        startState: "init",
        transitions: transitions,
        data: data,
        setup: setup,
        teardown: teardown,
    };
})();
