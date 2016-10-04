'use strict';

load('jstests/concurrency/fsm_workload_helpers/server_types.js');  // for isMongod

/**
 * yield.js
 *
 * Designed to execute queries and make them yield as much as possible while also updating and
 * removing documents that they operate on.
 */
var $config = (function() {

    var data = {
        // Number of docs to insert at the beginning.
        nDocs: 200,
        // Batch size of queries to introduce more saving and restoring of states.
        batchSize: 3,
        // The words that can be found in the collection.
        words: ['these', 'are', 'test', 'words'],
        /*
         * Helper function to advance a cursor, and verify that the documents that come out are
         * what we'd expect.
         */
        advanceCursor: function advanceCursor(cursor, verifier) {
            // Keep track of the previous doc in case the verifier is trying to verify a sorted
            // query.
            var prevDoc = null;
            var doc = null;
            while (cursor.hasNext()) {
                prevDoc = doc;
                doc = cursor.next();
                assertAlways(verifier(doc, prevDoc),
                             'Verifier failed!\nQuery: ' + tojson(cursor._query) + '\n' +
                                 'Query plan: ' + tojson(cursor.explain()) + '\n' +
                                 'Previous doc: ' + tojson(prevDoc) + '\n' +
                                 'This doc: ' + tojson(doc));
            }
            assertAlways.eq(cursor.itcount(), 0);
        },
        /*
         * Many subclasses will want different behavior in the update stage. To change what types
         * of updates happen, they can simply override this function to return which update doc
         * the update state should use for the update query.
         */
        genUpdateDoc: function genUpdateDoc() {
            var newVal = Random.randInt(this.nDocs);
            return {$set: {a: newVal}};
        }
    };

    var states = {
        /*
         * Update a random document from the collection.
         */
        update: function update(db, collName) {
            var id = Random.randInt(this.nDocs);
            var randDoc = db[collName].findOne({_id: id});
            if (randDoc === null) {
                return;
            }
            var updateDoc = this.genUpdateDoc();
            assertAlways.writeOK(db[collName].update(randDoc, updateDoc));
        },

        /*
         * Remove a random document from the collection, then re-insert one to prevent losing
         * documents.
         */
        remove: function remove(db, collName) {
            var id = Random.randInt(this.nDocs);
            var doc = db[collName].findOne({_id: id});
            if (doc !== null) {
                var res = db[collName].remove({_id: id});
                assertAlways.writeOK(res);
                if (res.nRemoved > 0) {
                    assertAlways.writeOK(db[collName].insert(doc));
                }
            }
        },

        /*
         * Issue a query that will potentially yield and resume while documents are being updated.
         * Subclasses will implement this differently
         */
        query: function collScan(db, collName) {
            var nMatches = 100;
            var cursor = db[collName].find({a: {$lt: nMatches}}).batchSize(this.batchSize);
            var collScanVerifier = function collScanVerifier(doc, prevDoc) {
                return doc.a < nMatches;
            };

            this.advanceCursor(cursor, collScanVerifier);
        }
    };

    /*
     * Visual of FSM:
     *
     *            _
     *           / \
     *           V /
     *          remove
     *          ^    ^
     *         /      \
     *        v       v
     * -->update<---->query
     *     ^ \           ^ \
     *     \_/           \_/
     *
     */
    var transitions = {
        update: {update: 0.334, remove: 0.333, query: 0.333},
        remove: {update: 0.333, remove: 0.334, query: 0.333},
        query: {update: 0.333, remove: 0.333, query: 0.334}
    };

    /*
     * Sets up the indices, sets a failpoint and lowers some yielding parameters to encourage
     * more yielding, and inserts the documents to be used.
     */
    function setup(db, collName, cluster) {
        // Enable this failpoint to trigger more yields. In MMAPV1, if a record fetch is about to
        // page fault, the query will yield. This failpoint will mock page faulting on such
        // fetches every other time.

        cluster.executeOnMongodNodes(function enableFailPoint(db) {
            assertAlways.commandWorked(
                db.adminCommand({configureFailPoint: 'recordNeedsFetchFail', mode: 'alwaysOn'}));
        });

        // Lower the following parameters to force even more yields.
        cluster.executeOnMongodNodes(function lowerYieldParams(db) {
            assertAlways.commandWorked(
                db.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 5}));
            assertAlways.commandWorked(
                db.adminCommand({setParameter: 1, internalQueryExecYieldPeriodMS: 1}));
        });
        // Set up some data to query.
        var N = this.nDocs;
        var bulk = db[collName].initializeUnorderedBulkOp();
        for (var i = 0; i < N; i++) {
            // Give each doc some word of text
            var word = this.words[i % this.words.length];
            bulk.find({_id: i}).upsert().updateOne(
                {$set: {a: i, b: N - i, c: i, d: N - i, yield_text: word}});
        }
        assertAlways.writeOK(bulk.execute());
    }

    /*
     * Reset parameters and disable failpoint.
     */
    function teardown(db, collName, cluster) {
        cluster.executeOnMongodNodes(function disableFailPoint(db) {
            assertAlways.commandWorked(
                db.adminCommand({configureFailPoint: 'recordNeedsFetchFail', mode: 'off'}));
        });
        cluster.executeOnMongodNodes(function resetYieldParams(db) {
            assertAlways.commandWorked(
                db.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 128}));
            assertAlways.commandWorked(
                db.adminCommand({setParameter: 1, internalQueryExecYieldPeriodMS: 10}));
        });
    }

    return {
        threadCount: 5,
        iterations: 50,
        startState: 'update',
        states: states,
        transitions: transitions,
        setup: setup,
        teardown: teardown,
        data: data
    };

})();
