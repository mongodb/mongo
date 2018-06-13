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
load('jstests/concurrency/fsm_libs/extend_workload.js');           // for extendWorkload
load('jstests/concurrency/fsm_workloads/invalidated_cursors.js');  // for $config

var $config = extendWorkload($config, function($config, $super) {

    /**
     * Runs the specified aggregation pipeline and stores the resulting cursor (if the command
     * is successful) in 'this.cursor'.
     */
    $config.data.runAggregation = function runAggregation(db, collName, pipeline) {
        var res = db.runCommand(
            {aggregate: collName, pipeline: pipeline, cursor: {batchSize: this.batchSize}});

        if (res.ok) {
            this.cursor = new DBCommandCursor(db, res, this.batchSize);
        }
    };

    $config.data.makeLookupPipeline = function makeLookupPipeline(foreignColl) {
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
    };

    $config.data.makeGraphLookupPipeline = function makeGraphLookupPipeline(foreignName) {
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
    };

    /**
     * Runs an aggregation involving only one collection and saves the resulting cursor to
     * 'this.cursor'.
     */
    $config.states.normalAggregation = function normalAggregation(db, collName) {
        var myDB = db.getSiblingDB(this.uniqueDBName);
        var targetCollName = this.chooseRandomlyFrom(this.involvedCollections);

        var pipeline = [{$sort: this.chooseRandomlyFrom(this.indexSpecs)}];
        this.runAggregation(myDB, targetCollName, pipeline);
    };

    /**
     * Runs an aggregation that uses the $lookup stage and saves the resulting cursor to
     * 'this.cursor'.
     */
    $config.states.lookupAggregation = function lookupAggregation(db, collName) {
        var myDB = db.getSiblingDB(this.uniqueDBName);
        var targetCollName = this.chooseRandomlyFrom(this.involvedCollections);
        var foreignCollName = this.chooseRandomlyFrom(this.involvedCollections);

        var pipeline = this.makeLookupPipeline(foreignCollName);
        this.runAggregation(myDB, targetCollName, pipeline);
    };

    /**
     * Runs an aggregation that uses the $graphLookup stage and saves the resulting cursor to
     * 'this.cursor'.
     */
    $config.states.graphLookupAggregation = function graphLookupAggregation(db, collName) {
        var myDB = db.getSiblingDB(this.uniqueDBName);
        var targetCollName = this.chooseRandomlyFrom(this.involvedCollections);
        var foreignCollName = this.chooseRandomlyFrom(this.involvedCollections);

        var pipeline = this.makeGraphLookupPipeline(foreignCollName);
        this.runAggregation(myDB, targetCollName, pipeline);
    };

    /**
     * Runs an aggregation that uses the $lookup and $graphLookup stages within a $facet stage
     * and saves the resulting cursor to 'this.cursor'.
     */
    $config.states.facetAggregation = function facetAggregation(db, collName) {
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
    };

    $config.transitions = {
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

    $config.setup = function setup(db, collName, cluster) {
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
    };

    $config.teardown = function teardown(db, collName, cluster) {
        cluster.executeOnMongodNodes(function lowerDocumentSourceCursorBatchSize(db) {
            // Restore DocumentSourceCursor batch size to the default.
            assertAlways.commandWorked(db.adminCommand(
                {setParameter: 1, internalDocumentSourceCursorBatchSizeBytes: 4 * 1024 * 1024}));
        });
    };

    return $config;
});
