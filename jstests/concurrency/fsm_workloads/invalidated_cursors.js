'use strict';

/**
 * invalidated_cursors.js
 *
 * This workload was designed to stress creating, pinning, and invalidating cursors through the
 * cursor manager. Threads perform find, getMore and explain commands while the database,
 * collection, or an index is dropped.
 */

load('jstests/concurrency/fsm_workload_helpers/server_types.js');  // for isMongos

var $config = (function() {

    let data = {
        chooseRandomlyFrom: function chooseRandomlyFrom(arr) {
            if (!Array.isArray(arr)) {
                throw new Error('Expected array for first argument, but got: ' + tojson(arr));
            }
            return arr[Random.randInt(arr.length)];
        },

        involvedCollections: ['coll0', 'coll1', 'coll2'],
        indexSpecs: [{a: 1, b: 1}, {c: 1}],

        numDocs: 100,
        batchSize: 2,

        /**
         * Inserts 'this.numDocs' new documents into the specified collection and ensures that the
         * indexes 'this.indexSpecs' exist on the collection. Note that means it is safe for
         * multiple threads to perform this function simultaneously.
         */
        populateDataAndIndexes: function populateDataAndIndexes(db, collName) {
            let bulk = db[collName].initializeUnorderedBulkOp();
            for (let i = 0; i < this.numDocs; ++i) {
                bulk.insert({});
            }
            let res = bulk.execute();
            assertAlways.writeOK(res);
            assertAlways.eq(this.numDocs, res.nInserted, tojson(res));

            this.indexSpecs.forEach(indexSpec => {
                assertAlways.commandWorked(db[collName].createIndex(indexSpec));
            });
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
        query: function query(db, collName) {
            let myDB = db.getSiblingDB(this.uniqueDBName);
            let res = myDB.runCommand({
                find: this.chooseRandomlyFrom(this.involvedCollections),
                filter: {},
                batchSize: this.batchSize
            });

            if (res.ok) {
                this.cursor = new DBCommandCursor(db, res, this.batchSize);
            }
        },

        /**
         * Explains a find on a collection.
         */
        explain: function explain(db, collName) {
            let myDB = db.getSiblingDB(this.uniqueDBName);
            let res = myDB.runCommand({
                explain: {find: this.chooseRandomlyFrom(this.involvedCollections), filter: {}},
                verbosity: "executionStats"
            });
            assertAlways.commandWorked(res);
        },

        killCursor: function killCursor(db, collName) {
            if (this.hasOwnProperty('cursor')) {
                this.cursor.close();
            }
        },

        /**
         * Requests enough results from 'this.cursor' to ensure that another batch is needed, and
         * thus ensures that a getMore request is sent for 'this.cursor'.
         */
        getMore: function getMore(db, collName) {
            if (!this.hasOwnProperty('cursor')) {
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
                    // dropped.
                    assertAlways.contains(e.code,
                                          [
                                            ErrorCodes.OperationFailed,
                                            ErrorCodes.QueryPlanKilled,
                                            ErrorCodes.CursorNotFound
                                          ],
                                          'unexpected error code: ' + e.code + ': ' + e.message);
                }
            }
        },

        /**
         * Drops the database being used by this workload and then re-creates each of
         * 'this.involvedCollections' by repopulating them with data and indexes.
         */
        dropDatabase: function dropDatabase(db, collName) {
            if (isMongos(db)) {
                // SERVER-17397: Drops in a sharded cluster may not fully succeed. It is not safe
                // to drop and then recreate a collection with the same name, so we skip dropping
                // and recreating the database.
                return;
            }

            let myDB = db.getSiblingDB(this.uniqueDBName);
            myDB.dropDatabase();

            // Re-create all of the collections and indexes that were dropped.
            this.involvedCollections.forEach(collName => {
                this.populateDataAndIndexes(myDB, collName);
            });
        },

        /**
         * Randomly selects a collection from 'this.involvedCollections' and drops it. The
         * collection is then re-created with data and indexes.
         */
        dropCollection: function dropCollection(db, collName) {
            if (isMongos(db)) {
                // SERVER-17397: Drops in a sharded cluster may not fully succeed. It is not safe
                // to drop and then recreate a collection with the same name, so we skip it.
                return;
            }

            let myDB = db.getSiblingDB(this.uniqueDBName);
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
        dropIndex: function dropIndex(db, collName) {
            let myDB = db.getSiblingDB(this.uniqueDBName);
            let targetColl = this.chooseRandomlyFrom(this.involvedCollections);
            let indexSpec = this.chooseRandomlyFrom(this.indexSpecs);

            // We don't assert that the command succeeded when dropping an index because it's
            // possible another thread has already dropped this index.
            myDB[targetColl].dropIndex(indexSpec);

            // Re-create the index that was dropped.
            assertAlways.commandWorked(myDB[targetColl].createIndex(indexSpec));
        }
    };

    let transitions = {
        init: {
            query: 0.6,
            explain: 0.1,
            dropDatabase: 0.1,
            dropCollection: 0.1,
            dropIndex: 0.1,
        },

        query: {killCursor: 0.1, getMore: 0.9},
        explain: {explain: 0.1, init: 0.9},
        killCursor: {init: 1},
        getMore: {killCursor: 0.2, getMore: 0.6, init: 0.2},
        dropDatabase: {init: 1},
        dropCollection: {init: 1},
        dropIndex: {init: 1}
    };

    function setup(db, collName, cluster) {
        // Use the workload name as part of the database name, since the workload name is assumed to
        // be unique.
        this.uniqueDBName = db.getName() + 'invalidated_cursors';

        let myDB = db.getSiblingDB(this.uniqueDBName);
        this.involvedCollections.forEach(collName => {
            this.populateDataAndIndexes(myDB, collName);
            assertAlways.eq(this.numDocs, myDB[collName].find({}).itcount());
        });
    }

    function teardown(db, collName, cluster) {
        db.getSiblingDB(this.uniqueDBName).dropDatabase();
    }

    return {
        threadCount: 10,
        iterations: 200,
        states: states,
        startState: 'init',
        transitions: transitions,
        data: data,
        setup: setup,
        teardown: teardown
    };
})();
