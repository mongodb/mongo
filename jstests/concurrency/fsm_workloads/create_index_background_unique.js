'use strict';

/**
 * create_index_background_unique.js
 *
 * Creates multiple unique background indexes in parallel.
 */

load('jstests/concurrency/fsm_workload_helpers/drop_utils.js');  // for dropCollections

var $config = (function() {

    var data = {
        prefix: "create_index_background_unique_",
        numDocsToLoad: 5000,
        iterationCount: 0,
        getCollectionNameForThread: function(threadId) {
            return this.prefix + threadId.toString();
        },
        // Allows tests that inherit from this one to specify options other than the default.
        getCollectionOptions: function() {
            return {};
        },
        buildvariableSizedDoc: function(uniquePrefix) {
            const indexedVal = uniquePrefix + Array(Random.randInt(1000)).toString();
            const doc = {x: indexedVal};
            return doc;
        },
    };

    var states = (function() {
        function buildIndex(db, collName) {
            this.iterationCount++;

            const res = db.runCommand({
                createIndexes: this.getCollectionNameForThread(this.tid),
                indexes: [{key: {x: 1}, name: "x_1", unique: true, background: true}]
            });
            assertAlways.commandWorked(res);
        }

        function dropIndex(db, collName) {
            this.iterationCount++;

            // In the case that we have an even # of iterations, we skip the final drop so that
            // validation can be performed on the indexes created.
            if (this.iterationCount === this.iterations) {
                return;
            }

            assertAlways.commandWorked(db.runCommand(
                {dropIndexes: this.getCollectionNameForThread(this.tid), index: "x_1"}));
        }

        return {
            buildIndex: buildIndex,
            dropIndex: dropIndex,
        };

    })();

    var transitions = {
        buildIndex: {dropIndex: 1.0},
        dropIndex: {buildIndex: 1.0},
    };

    function setup(db, collName, cluster) {
        for (let j = 0; j < this.threadCount; ++j) {
            const collectionName = this.getCollectionNameForThread(j);
            assertAlways.commandWorked(
                db.createCollection(collectionName, this.getCollectionOptions()));
            var bulk = db[collectionName].initializeUnorderedBulkOp();

            // Preload documents for each thread's collection. This ensures that the index build and
            // drop have meaningful work to do.
            for (let i = 0; i < this.numDocsToLoad; ++i) {
                const uniqueValuePrefix = i.toString() + "_";
                bulk.insert(this.buildvariableSizedDoc(uniqueValuePrefix));
            }
            assertAlways.writeOK(bulk.execute());
            assertAlways.eq(this.numDocsToLoad, db[collectionName].find({}).itcount());
        }
    }

    function teardown(db, collName, cluster) {
        // Drop all collections from this workload.
        const pattern = new RegExp('^' + this.prefix + '[0-9]*$');
        dropCollections(db, pattern);
    }

    return {
        threadCount: 10,
        iterations: 11,
        data: data,
        states: states,
        startState: 'buildIndex',
        transitions: transitions,
        setup: setup,
        teardown: teardown,
    };
})();
