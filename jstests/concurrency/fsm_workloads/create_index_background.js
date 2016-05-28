'use strict';

/**
 * create_index_background.js
 *
 * Create an index in the background while performing CRUD operations at the same time.
 * The command to create a background index completes in the shell once the
 * index has completed and the test no longer needs to execute more transitions.
 * The first thread (tid = 0) will be the one that creates the background index.
 */
load('jstests/concurrency/fsm_workload_helpers/server_types.js');  // for isMongos

var $config = (function() {

    var data = {
        nDocumentsToSeed: 1000,
        nDocumentsToCreate: 200,
        nDocumentsToRead: 100,
        nDocumentsToUpdate: 50,
        nDocumentsToDelete: 50,
        getHighestX: function getHighestX(coll, tid) {
            // Find highest value of x.
            var highest = 0;
            var cursor = coll.find({tid: tid}).sort({x: -1}).limit(-1);
            assertWhenOwnColl(function() {
                highest = cursor.next().x;
            });
            return highest;
        }
    };

    var states = (function() {

        function init(db, collName) {
            // Add thread-specific documents
            var bulk = db[collName].initializeUnorderedBulkOp();
            for (var i = 0; i < this.nDocumentsToSeed; ++i) {
                bulk.insert({x: i, tid: this.tid});
            }
            var res = bulk.execute();
            assertAlways.writeOK(res);
            assertAlways.eq(this.nDocumentsToSeed, res.nInserted, tojson(res));

            // In the first thread create the background index.
            if (this.tid === 0) {
                var coll = db[collName];
                // Before creating the background index make sure insert or update
                // CRUD operations are active.
                assertWhenOwnColl.soon(function() {
                    return coll.find({crud: {$exists: true}}).itcount() > 0;
                }, 'No documents with "crud" field have been inserted or updated', 60 * 1000);
                res = coll.ensureIndex({x: 1}, {background: true});
                assertAlways.commandWorked(res, tojson(res));
            }
        }

        function createDocs(db, collName) {
            // Insert documents with an increasing value of index x.
            var coll = db[collName];
            var res;
            var count = coll.find({tid: this.tid}).itcount();

            var highest = this.getHighestX(coll, this.tid);
            for (var i = 0; i < this.nDocumentsToCreate; ++i) {
                res = coll.insert({x: i + highest + 1, tid: this.tid, crud: 1});
                assertAlways.writeOK(res);
                assertAlways.eq(res.nInserted, 1, tojson(res));
            }
            assertWhenOwnColl.eq(coll.find({tid: this.tid}).itcount(),
                                 this.nDocumentsToCreate + count,
                                 'createDocs itcount mismatch');
        }

        function readDocs(db, collName) {
            // Read random documents from the collection on index x.
            var coll = db[collName];
            var res;
            var count = coll.find({tid: this.tid}).itcount();
            assertWhenOwnColl.gte(
                count, this.nDocumentsToRead, 'readDocs not enough documents for tid ' + this.tid);

            var highest = this.getHighestX(coll, this.tid);
            for (var i = 0; i < this.nDocumentsToRead; ++i) {
                // Do randomized reads on index x. A document is not guaranteed
                // to match the randomized 'x' predicate.
                res = coll.find({x: Random.randInt(highest), tid: this.tid}).itcount();
                assertWhenOwnColl.contains(res, [0, 1], tojson(res));
            }
            assertWhenOwnColl.eq(
                coll.find({tid: this.tid}).itcount(), count, 'readDocs itcount mismatch');
        }

        function updateDocs(db, collName) {
            // Update random documents from the collection on index x.
            // Since an update requires a shard key, do not run in a sharded cluster.
            if (!isMongos(db)) {
                var coll = db[collName];
                var res;
                var count = coll.find({tid: this.tid}).itcount();
                assertWhenOwnColl.gte(count,
                                      this.nDocumentsToUpdate,
                                      'updateDocs not enough documents for tid ' + this.tid);

                var highest = this.getHighestX(coll, this.tid);
                for (var i = 0; i < this.nDocumentsToUpdate; ++i) {
                    // Do randomized updates on index x. A document is not guaranteed
                    // to match the randomized 'x' predicate.
                    res =
                        coll.update({x: Random.randInt(highest), tid: this.tid}, {$inc: {crud: 1}});
                    assertAlways.writeOK(res);
                    if (db.getMongo().writeMode() === 'commands') {
                        assertWhenOwnColl.contains(res.nModified, [0, 1], tojson(res));
                    }
                    assertWhenOwnColl.contains(res.nMatched, [0, 1], tojson(res));
                    assertWhenOwnColl.eq(res.nUpserted, 0, tojson(res));
                }
                assertWhenOwnColl.eq(
                    coll.find({tid: this.tid}).itcount(), count, 'updateDocs itcount mismatch');
            }
        }

        function deleteDocs(db, collName) {
            // Remove random documents from the collection on index x.
            var coll = db[collName];
            var res;
            var count = coll.find({tid: this.tid}).itcount();

            // Set the maximum number of documents we can delete to ensure that there
            // are documents to read or update after deleteDocs completes.
            // Return from this state if there are not enough documents in the collection
            // with this.tid.

            var minDocsToKeep = Math.max(this.nDocumentsToRead, this.nDocumentsToUpdate);
            // nDeleteDocs is the number of documents that can be deleted during this state.
            var nDeleteDocs = Math.min(count - minDocsToKeep, this.nDocumentsToDelete);
            if (nDeleteDocs < 0) {
                return;
            }

            var highest = this.getHighestX(coll, this.tid);
            var nActualDeletes = 0;
            for (var i = 0; i < nDeleteDocs; ++i) {
                // Do randomized deletes on index x. A document is not guaranteed
                // to match the randomized 'x' predicate.
                res = coll.remove({x: Random.randInt(highest), tid: this.tid});
                assertAlways.writeOK(res);
                assertWhenOwnColl.contains(res.nRemoved, [0, 1], tojson(res));
                nActualDeletes += res.nRemoved;
            }
            assertWhenOwnColl.eq(coll.find({tid: this.tid}).itcount(),
                                 count - nActualDeletes,
                                 'deleteDocs itcount mismatch');
        }

        return {
            init: init,
            createDocs: createDocs,
            readDocs: readDocs,
            updateDocs: updateDocs,
            deleteDocs: deleteDocs
        };

    })();

    var transitions = {
        init: {createDocs: 1},
        createDocs: {createDocs: 0.25, readDocs: 0.25, updateDocs: 0.25, deleteDocs: 0.25},
        readDocs: {createDocs: 0.25, readDocs: 0.25, updateDocs: 0.25, deleteDocs: 0.25},
        updateDocs: {createDocs: 0.25, readDocs: 0.25, updateDocs: 0.25, deleteDocs: 0.25},
        deleteDocs: {createDocs: 0.25, readDocs: 0.25, updateDocs: 0.25, deleteDocs: 0.25},
    };

    var internalQueryExecYieldIterations;
    var internalQueryExecYieldPeriodMS;

    function setup(db, collName, cluster) {
        var nSetupDocs = this.nDocumentsToSeed * 200;
        var coll = db[collName];

        var res = coll.ensureIndex({tid: 1});
        assertAlways.commandWorked(res, tojson(res));

        var bulk = coll.initializeUnorderedBulkOp();
        for (var i = 0; i < nSetupDocs; ++i) {
            bulk.insert({x: i});
        }
        res = bulk.execute();
        assertAlways.writeOK(res);
        assertAlways.eq(nSetupDocs, res.nInserted, tojson(res));

        // Increase the following parameters to reduce the number of yields.
        cluster.executeOnMongodNodes(function(db) {
            var res;
            res = db.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 100000});
            assertAlways.commandWorked(res);
            internalQueryExecYieldIterations = res.was;

            res = db.adminCommand({setParameter: 1, internalQueryExecYieldPeriodMS: 10000});
            assertAlways.commandWorked(res);
            internalQueryExecYieldPeriodMS = res.was;
        });
    }

    function teardown(db, collName, cluster) {
        cluster.executeOnMongodNodes(function(db) {
            assertAlways.commandWorked(db.adminCommand({
                setParameter: 1,
                internalQueryExecYieldIterations: internalQueryExecYieldIterations
            }));
            assertAlways.commandWorked(db.adminCommand(
                {setParameter: 1, internalQueryExecYieldPeriodMS: internalQueryExecYieldPeriodMS}));
        });
    }

    return {
        threadCount: 5,
        iterations: 3,
        data: data,
        states: states,
        setup: setup,
        teardown: teardown,
        transitions: transitions,
    };

})();
