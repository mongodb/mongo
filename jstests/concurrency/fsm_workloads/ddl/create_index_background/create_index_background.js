/**
 * create_index_background.js
 *
 * Create an index in the background while performing CRUD operations at the same time.
 * The command to create a background index completes in the shell once the
 * index has completed and the test no longer needs to execute more transitions.
 * The first thread (tid = 0) will be the one that creates the background index.
 *
 * This workload implicitly assumes that its tid ranges are [0, $config.threadCount). This
 * isn't guaranteed to be true when they are run in parallel with other workloads. Therefore
 * it can't be run in concurrency simultaneous suites.
 * @tags: [
 *   assumes_balancer_off,
 *   creates_background_indexes,
 *   requires_getmore,
 *   incompatible_with_concurrency_simultaneous,
 * ]
 */
import {isMongos} from "jstests/concurrency/fsm_workload_helpers/server_types.js";

export const $config = (function () {
    let data = {
        nDocumentsToSeed: 500,
        nDocumentsToCreate: 100,
        nDocumentsToRead: 100,
        nDocumentsToUpdate: 50,
        nDocumentsToDelete: 50,
        getHighestX: function getHighestX(coll, tid) {
            // Find highest value of x.
            let highest = 0;
            let cursor = coll.find({tid: tid}).sort({x: -1}).limit(-1);
            highest = cursor.next().x;
            return highest;
        },
        getPartialFilterExpression: function getPartialFilterExpression() {
            return undefined;
        },
        getIndexSpec: function getIndexSpec() {
            return {x: 1};
        },
        extendDocument: function getDocument(originalDocument) {
            // Only relevant for extended workloads.
            return originalDocument;
        },
        extendUpdateExpr: function extendUpdateExpr(update) {
            // Only relevant for extended workloads.
            return update;
        },
    };

    let states = (function () {
        function init(db, collName) {
            // Add thread-specific documents
            let bulk = db[collName].initializeUnorderedBulkOp();
            for (let i = 0; i < this.nDocumentsToSeed; ++i) {
                const doc = {x: i, tid: this.tid};
                bulk.insert(this.extendDocument(doc));
            }
            let res = bulk.execute();
            assert.commandWorked(res);
            assert.eq(this.nDocumentsToSeed, res.nInserted, tojson(res));

            // In the first thread create the background index.
            if (this.tid === 0) {
                let coll = db[collName];
                // Before creating the background index make sure insert or update
                // CRUD operations are active.
                assert.soon(
                    function () {
                        return coll.find({crud: {$exists: true}}).itcount() > 0;
                    },
                    'No documents with "crud" field have been inserted or updated',
                    60 * 1000,
                );

                let createOptions = {};
                let filter = this.getPartialFilterExpression();
                if (filter !== undefined) {
                    createOptions["partialFilterExpression"] = filter;
                }

                res = coll.createIndex(this.getIndexSpec(), createOptions);
                assert.commandWorked(res, tojson(res));
            }
        }

        function createDocs(db, collName) {
            // Insert documents with an increasing value of index x.
            let coll = db[collName];
            let res;
            let count = coll.find({tid: this.tid}).itcount();

            let highest = this.getHighestX(coll, this.tid);
            for (let i = 0; i < this.nDocumentsToCreate; ++i) {
                const doc = {x: i + highest + 1, tid: this.tid, crud: 1};
                res = coll.insert(this.extendDocument(doc));
                assert.commandWorked(res);
                assert.eq(res.nInserted, 1, tojson(res));
            }
            assert.eq(
                coll.find({tid: this.tid}).itcount(),
                this.nDocumentsToCreate + count,
                "createDocs itcount mismatch",
            );
        }

        function readDocs(db, collName) {
            // Read random documents from the collection on index x.
            let coll = db[collName];
            let res;
            let count = coll.find({tid: this.tid}).itcount();
            assert.gte(count, this.nDocumentsToRead, "readDocs not enough documents for tid " + this.tid);

            let highest = this.getHighestX(coll, this.tid);
            for (let i = 0; i < this.nDocumentsToRead; ++i) {
                // Do randomized reads on index x. A document is not guaranteed
                // to match the randomized 'x' predicate.
                res = coll.find({x: Random.randInt(highest), tid: this.tid}).itcount();
                assert.contains(res, [0, 1], tojson(res));
            }
            assert.eq(coll.find({tid: this.tid}).itcount(), count, "readDocs itcount mismatch");
        }

        function updateDocs(db, collName) {
            // Update random documents from the collection on index x.
            // Since an update requires a shard key, do not run in a sharded cluster.
            if (!isMongos(db)) {
                let coll = db[collName];
                let res;
                let count = coll.find({tid: this.tid}).itcount();
                assert.gte(count, this.nDocumentsToUpdate, "updateDocs not enough documents for tid " + this.tid);

                let highest = this.getHighestX(coll, this.tid);
                for (let i = 0; i < this.nDocumentsToUpdate; ++i) {
                    // Do randomized updates on index x. A document is not guaranteed
                    // to match the randomized 'x' predicate.

                    let updateExpr = {$inc: {crud: 1}};
                    updateExpr = this.extendUpdateExpr(updateExpr);

                    res = coll.update({x: Random.randInt(highest), tid: this.tid}, updateExpr);
                    assert.commandWorked(res);
                    assert.contains(res.nModified, [0, 1], tojson(res));
                    assert.contains(res.nMatched, [0, 1], tojson(res));
                    assert.eq(res.nUpserted, 0, tojson(res));
                }
                assert.eq(coll.find({tid: this.tid}).itcount(), count, "updateDocs itcount mismatch");
            }
        }

        function deleteDocs(db, collName) {
            // Remove random documents from the collection on index x.
            let coll = db[collName];
            let res;
            let count = coll.find({tid: this.tid}).itcount();

            // Set the maximum number of documents we can delete to ensure that there
            // are documents to read or update after deleteDocs completes.
            // Return from this state if there are not enough documents in the collection
            // with this.tid.

            let minDocsToKeep = Math.max(this.nDocumentsToRead, this.nDocumentsToUpdate);
            // nDeleteDocs is the number of documents that can be deleted during this state.
            let nDeleteDocs = Math.min(count - minDocsToKeep, this.nDocumentsToDelete);
            if (nDeleteDocs < 0) {
                return;
            }

            let highest = this.getHighestX(coll, this.tid);
            let nActualDeletes = 0;
            for (let i = 0; i < nDeleteDocs; ++i) {
                // Do randomized deletes on index x. A document is not guaranteed
                // to match the randomized 'x' predicate.
                res = coll.remove({x: Random.randInt(highest), tid: this.tid});
                assert.commandWorked(res);
                assert.contains(res.nRemoved, [0, 1], tojson(res));
                nActualDeletes += res.nRemoved;
            }
            assert.eq(coll.find({tid: this.tid}).itcount(), count - nActualDeletes, "deleteDocs itcount mismatch");
        }

        return {
            init: init,
            createDocs: createDocs,
            readDocs: readDocs,
            updateDocs: updateDocs,
            deleteDocs: deleteDocs,
        };
    })();

    let transitions = {
        init: {createDocs: 1},
        createDocs: {createDocs: 0.25, readDocs: 0.25, updateDocs: 0.25, deleteDocs: 0.25},
        readDocs: {createDocs: 0.25, readDocs: 0.25, updateDocs: 0.25, deleteDocs: 0.25},
        updateDocs: {createDocs: 0.25, readDocs: 0.25, updateDocs: 0.25, deleteDocs: 0.25},
        deleteDocs: {createDocs: 0.25, readDocs: 0.25, updateDocs: 0.25, deleteDocs: 0.25},
    };

    let internalQueryExecYieldIterations;
    let internalQueryExecYieldPeriodMS;

    function setup(db, collName, cluster) {
        let nSetupDocs = this.nDocumentsToSeed * 50;
        let coll = db[collName];

        let res = coll.createIndex({tid: 1});
        assert.commandWorked(res, tojson(res));

        let bulk = coll.initializeUnorderedBulkOp();
        for (let i = 0; i < nSetupDocs; ++i) {
            bulk.insert({x: i});
        }
        res = bulk.execute();
        assert.commandWorked(res);
        assert.eq(nSetupDocs, res.nInserted, tojson(res));

        // Increase the following parameters to reduce the number of yields.
        cluster.executeOnMongodNodes(function (db) {
            let res;
            res = db.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 100000});
            assert.commandWorked(res);
            internalQueryExecYieldIterations = res.was;

            res = db.adminCommand({setParameter: 1, internalQueryExecYieldPeriodMS: 10000});
            assert.commandWorked(res);
            internalQueryExecYieldPeriodMS = res.was;
        });
    }

    function teardown(db, collName, cluster) {
        cluster.executeOnMongodNodes(function (db) {
            assert.commandWorked(
                db.adminCommand({
                    setParameter: 1,
                    internalQueryExecYieldIterations: internalQueryExecYieldIterations,
                }),
            );
            assert.commandWorked(
                db.adminCommand({setParameter: 1, internalQueryExecYieldPeriodMS: internalQueryExecYieldPeriodMS}),
            );
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
