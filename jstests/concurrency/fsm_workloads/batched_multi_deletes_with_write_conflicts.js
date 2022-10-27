'use strict';
load("jstests/libs/analyze_plan.js");

/**
 * batched_multi_deletes_with_write_conflicts.js
 *
 * Tests batched deletes concurrenctly with other write operations on the same documents.
 *
 * @tags: [
 *  does_not_support_retryable_writes,
 *  requires_non_retryable_writes,
 *  does_not_support_transactions,
 *  requires_fcv_61,
 * ]
 */

var $config = (function() {
    // 'data' is passed (copied) to each of the worker threads.
    var data = {
        // Defines the number of subsets of data, which are randomly picked to create conflicts.
        numInsertSubsets: 5,
        // Docs in each subset. insertSize = subsetSize x numInsertSubsets.
        subsetSize: 50,
    };

    // 'setup' is run once by the parent thread after the cluster has
    // been initialized, but before the worker threads have been spawned.
    // The 'this' argument is bound as '$config.data'. 'cluster' is provided
    // to allow execution against all mongos and mongod nodes.
    function setup(db, collName, cluster) {
        // Workloads should NOT drop the collection db[collName], as
        // doing so is handled by runner.js before 'setup' is called.

        // Verify that the BATCHED_DELETE stage is used.
        {
            assert.commandWorked(db[collName].insert({}));

            const expl = db.runCommand({
                explain: {delete: collName, deletes: [{q: {_id: {$gte: 0}}, limit: 0}]},
                verbosity: "executionStats"
            });
            assert.commandWorked(expl);

            const clusterTopology = cluster.getSerializedCluster();
            if (clusterTopology === '') {
                assert(getPlanStage(expl, "BATCHED_DELETE"), tojson(expl));
            } else {
                const shardNames = Object.keys(clusterTopology.shards);
                const stages = getPlanStages(expl, "BATCHED_DELETE");
                assert.eq(stages.length, shardNames.length, tojson(expl));
            }
            assert.commandWorked(db[collName].remove({}));
        }

        assert.commandWorked(
            db[collName].createIndex({deleter_tid: 1, delete_query_match: 1, subset_id: 1}));
    }

    // 'teardown' is run once by the parent thread before the cluster
    // is destroyed, but after the worker threads have been reaped.
    // The 'this' argument is bound as '$config.data'. 'cluster' is provided
    // to allow execution against all mongos and mongod nodes.
    function teardown(db, collName, cluster) {
    }

    // 'states' are the different functions callable by a worker
    // thread. The 'this' argument of any exposed function is
    // bound as '$config.data'.
    var states = (function() {
        // Helpers
        function getRandomInRange({min, max}) {
            return Math.floor(Math.random() * (max - min + 1) + min);
        }
        function getRandomUpTo(max) {
            return getRandomInRange({min: 1, max: max});
        }

        // State functions
        function init(db, collName) {
        }

        /**
         * Inserts documents and batch deletes them.
         *
         * Document layout:
         *  deleter_tid: deleter thread id.
         *  delete_query_match: field used to generate update conflicts.
         *  subset_id: field used to select subset of data by conflict generators.
         *
         **/
        function batchedDelete(db, collName) {
            const coll = db[collName];

            const subsetTemplates =
                Array(this.numInsertSubsets)
                    .fill()
                    .map((_, i) =>
                             ({deleter_tid: this.tid, delete_query_match: true, subset_id: i + 1}));

            // Create array of (subsetSize * numInsertSubsets) docs, by repeating the
            // subsetTemplates baseInsertSize times.
            const docs = Array(this.subsetSize).fill(subsetTemplates).flat();
            assert.commandWorked(coll.insertMany(docs, {ordered: false}));

            // Do batched delete.
            const deleteResult = coll.deleteMany({deleter_tid: this.tid, delete_query_match: true});
            assert.commandWorked(deleteResult);
        }

        // Takes a random subset of documents potentially being batch deleted and updates them.
        function updateConflict(db, collName) {
            const updateRes = db[collName].updateMany({
                deleter_tid: {$ne: this.tid},  // Exclude self.
                delete_query_match: true,      // Select documents that might be being deleted.
                subset_id: getRandomUpTo(this.numInsertSubsets)  // Select subset.
            },
                                                      {$set: {delete_query_match: false}});
            assert.commandWorked(updateRes);
        }

        // Takes a random subset of documents potentially being batch deleted and re-delete them.
        function deleteConflict(db, collName) {
            const deleteResult = db[collName].deleteMany({
                deleter_tid: {$ne: this.tid},
                delete_query_match: true,
                subset_id: getRandomUpTo(this.numInsertSubsets)
            });
            assert.commandWorked(deleteResult);
        }

        return {
            init: init,
            batchedDelete: batchedDelete,
            updateConflict: updateConflict,
            deleteConflict: deleteConflict
        };
    })();

    // All transtions are equally probable, given that a normalized random value is selected.
    const transitionToAll = {batchedDelete: 1, deleteConflict: 1, updateConflict: 1};
    const transitions = {
        init: transitionToAll,
        batchedDelete: transitionToAll,
        updateConflict: transitionToAll,
        deleteConflict: transitionToAll
    };

    return {
        threadCount: 10,
        iterations: 50,
        startState: 'init',
        states: states,
        transitions: transitions,
        setup: setup,
        teardown: teardown,
        data: data
    };
})();
