'use strict';

/**
 * kill_multicollection_aggregation.js
 *
 * This workload was designed to stress running and invalidating aggregation pipelines involving
 * multiple collections and to reproduce issues like those described in SERVER-22537 and
 * SERVER-24386. Threads perform an aggregation pipeline on one of a few collections, optionally
 * specifying a $lookup stage, a $graphLookup stage, or a $facet stage, while the database, a
 * collection, or an index is dropped concurrently.
 */
var $config = (function() {

    var data = {
        chooseRandomlyFrom: function chooseRandomlyFrom(arr) {
            if (!Array.isArray(arr)) {
                throw new Error('Expected array for first argument, but got: ' + tojson(arr));
            }
            return arr[Random.randInt(arr.length)];
        },

        involvedCollections: ['coll0', 'coll1', 'coll2'],
        indexSpecs: [{a: 1, b: 1}, {c: 1}],

        numDocs: 10,
        batchSize: 2,

        /**
         * Inserts 'this.numDocs' new documents into the specified collection and ensures that the
         * indexes 'this.indexSpecs' exist on the collection. Note that means it is safe for
         * multiple threads to perform this function simultaneously.
         */
        populateDataAndIndexes: function populateDataAndIndexes(db, collName) {
            var bulk = db[collName].initializeUnorderedBulkOp();
            for (var i = 0; i < this.numDocs; ++i) {
                bulk.insert({});
            }
            var res = bulk.execute();
            assertAlways.writeOK(res);
            assertAlways.eq(this.numDocs, res.nInserted, tojson(res));

            this.indexSpecs.forEach(indexSpec => {
                assertAlways.commandWorked(db[collName].createIndex(indexSpec));
            });
        },

        /**
         * Runs the specified aggregation pipeline and stores the resulting cursor (if the command
         * is successful) in 'this.cursor'.
         */
        runAggregation: function runAggregation(db, collName, pipeline) {
            var res = db.runCommand(
                {aggregate: collName, pipeline: pipeline, cursor: {batchSize: this.batchSize}});

            if (res.ok) {
                this.cursor = new DBCommandCursor(db.getMongo(), res, this.batchSize);
            }
        },

        makeLookupPipeline: function makeLookupPipeline(foreignColl) {
            var pipeline = [
                {
                  $lookup: {
                      from: foreignColl,
                      // We perform the $lookup on a field that doesn't exist in either the document
                      // on the source collection or the document on the foreign collection. This
                      // ensures that every document in the source collection will match every
                      // document in the foreign collection and cause the cursor underlying the
                      // $lookup stage to need to request another batch.
                      localField: 'fieldThatDoesNotExistInDoc',
                      foreignField: 'fieldThatDoesNotExistInDoc',
                      as: 'results'
                  }
                },
                {$unwind: '$results'}
            ];

            return pipeline;
        },

        makeGraphLookupPipeline: function makeGraphLookupPipeline(foreignName) {
            var pipeline = [
                {
                  $graphLookup: {
                      from: foreignName,
                      startWith: '$fieldThatDoesNotExistInDoc',
                      connectToField: 'fieldThatDoesNotExistInDoc',
                      connectFromField: 'fieldThatDoesNotExistInDoc',
                      maxDepth: Random.randInt(5),
                      as: 'results'
                  }
                },
                {$unwind: '$results'}
            ];

            return pipeline;
        }
    };

    var states = {
        /**
         * This is a no-op, used only as a transition state.
         */
        init: function init(db, collName) {},

        /**
         * Runs an aggregation involving only one collection and saves the resulting cursor to
         * 'this.cursor'.
         */
        normalAggregation: function normalAggregation(db, collName) {
            var myDB = db.getSiblingDB(this.uniqueDBName);
            var targetCollName = this.chooseRandomlyFrom(this.involvedCollections);

            var pipeline = [{$sort: this.chooseRandomlyFrom(this.indexSpecs)}];
            this.runAggregation(myDB, targetCollName, pipeline);
        },

        /**
         * Runs an aggregation that uses the $lookup stage and saves the resulting cursor to
         * 'this.cursor'.
         */
        lookupAggregation: function lookupAggregation(db, collName) {
            var myDB = db.getSiblingDB(this.uniqueDBName);
            var targetCollName = this.chooseRandomlyFrom(this.involvedCollections);
            var foreignCollName = this.chooseRandomlyFrom(this.involvedCollections);

            var pipeline = this.makeLookupPipeline(foreignCollName);
            this.runAggregation(myDB, targetCollName, pipeline);
        },

        /**
         * Runs an aggregation that uses the $graphLookup stage and saves the resulting cursor to
         * 'this.cursor'.
         */
        graphLookupAggregation: function graphLookupAggregation(db, collName) {
            var myDB = db.getSiblingDB(this.uniqueDBName);
            var targetCollName = this.chooseRandomlyFrom(this.involvedCollections);
            var foreignCollName = this.chooseRandomlyFrom(this.involvedCollections);

            var pipeline = this.makeGraphLookupPipeline(foreignCollName);
            this.runAggregation(myDB, targetCollName, pipeline);
        },

        /**
         * Runs an aggregation that uses the $lookup and $graphLookup stages within a $facet stage
         * and saves the resulting cursor to 'this.cursor'.
         */
        facetAggregation: function facetAggregation(db, collName) {
            var myDB = db.getSiblingDB(this.uniqueDBName);
            var targetCollName = this.chooseRandomlyFrom(this.involvedCollections);
            var lookupForeignCollName = this.chooseRandomlyFrom(this.involvedCollections);
            var graphLookupForeignCollName = this.chooseRandomlyFrom(this.involvedCollections);

            var pipeline = [{
                $facet: {
                    lookup: this.makeLookupPipeline(lookupForeignCollName),
                    graphLookup: this.makeGraphLookupPipeline(graphLookupForeignCollName)
                }
            }];
            this.runAggregation(myDB, targetCollName, pipeline);
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

            for (var i = 0; i <= this.batchSize; ++i) {
                try {
                    if (!this.cursor.hasNext()) {
                        break;
                    }
                    this.cursor.next();
                } catch (e) {
                    // The getMore request can fail if the database, a collection, or an index was
                    // dropped.
                }
            }
        },

        /**
         * Drops the database being used by this workload and then re-creates each of
         * 'this.involvedCollections' by repopulating them with data and indexes.
         */
        dropDatabase: function dropDatabase(db, collName) {
            var myDB = db.getSiblingDB(this.uniqueDBName);
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
            var myDB = db.getSiblingDB(this.uniqueDBName);
            var targetColl = this.chooseRandomlyFrom(this.involvedCollections);

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
            var myDB = db.getSiblingDB(this.uniqueDBName);
            var targetColl = this.chooseRandomlyFrom(this.involvedCollections);
            var indexSpec = this.chooseRandomlyFrom(this.indexSpecs);

            // We don't assert that the command succeeded when dropping an index because it's
            // possible another thread has already dropped this index.
            myDB[targetColl].dropIndex(indexSpec);

            // Re-create the index that was dropped.
            assertAlways.commandWorked(myDB[targetColl].createIndex(indexSpec));
        }
    };

    var transitions = {
        init: {
            normalAggregation: 0.1,
            lookupAggregation: 0.25,
            graphLookupAggregation: 0.25,
            facetAggregation: 0.25,
            dropDatabase: 0.05,
            dropCollection: 0.05,
            dropIndex: 0.05,
        },

        normalAggregation: {killCursor: 0.1, getMore: 0.9},
        lookupAggregation: {killCursor: 0.1, getMore: 0.9},
        graphLookupAggregation: {killCursor: 0.1, getMore: 0.9},
        facetAggregation: {killCursor: 0.1, getMore: 0.9},
        killCursor: {init: 1},
        getMore: {killCursor: 0.2, getMore: 0.6, init: 0.2},
        dropDatabase: {init: 1},
        dropCollection: {init: 1},
        dropIndex: {init: 1}
    };

    function setup(db, collName, cluster) {
        // We decrease the batch size of the DocumentSourceCursor so that the PlanExecutor
        // underlying it isn't exhausted when the "aggregate" command is sent. This makes it more
        // likely for the "killCursors" command to need to handle destroying the underlying
        // PlanExecutor.
        cluster.executeOnMongodNodes(function lowerDocumentSourceCursorBatchSize(db) {
            assertAlways.commandWorked(
                db.adminCommand({setParameter: 1, internalDocumentSourceCursorBatchSizeBytes: 1}));
        });

        // Use the workload name as part of the database name, since the workload name is assumed to
        // be unique.
        this.uniqueDBName = db.getName() + 'kill_multicollection_aggregation';

        var myDB = db.getSiblingDB(this.uniqueDBName);
        this.involvedCollections.forEach(collName => {
            this.populateDataAndIndexes(myDB, collName);
            assertAlways.eq(this.numDocs, myDB[collName].find({}).itcount());
        });
    }

    function teardown(db, collName, cluster) {
        cluster.executeOnMongodNodes(function lowerDocumentSourceCursorBatchSize(db) {
            // Restore DocumentSourceCursor batch size to the default.
            assertAlways.commandWorked(db.adminCommand(
                {setParameter: 1, internalDocumentSourceCursorBatchSizeBytes: 4 * 1024 * 1024}));
        });

        var myDB = db.getSiblingDB(this.uniqueDBName);
        myDB.dropDatabase();
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
