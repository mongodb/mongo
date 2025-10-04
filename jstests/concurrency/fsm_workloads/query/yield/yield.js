/**
 * yield.js
 *
 * Designed to execute queries and make them yield as much as possible while also updating and
 * removing documents that they operate on.
 * @tags: [
 *   requires_getmore,
 *   # Runs a multi-delete which is non-retryable.
 *   requires_non_retryable_writes,
 *   # This test relies on query commands returning specific batch-sized responses.
 *   assumes_no_implicit_cursor_exhaustion,
 * ]
 */

export const $config = (function () {
    // The explain used to build the assertion message in advanceCursor() is the only command not
    // allowed in a transaction used in the query state function. With shard stepdowns, getMores
    // aren't allowed outside a transaction, so if the explain runs when the suite is configured to
    // run with transactions and shard stepdowns, the query state function will be retried outside a
    // transaction, which fails the test. This can be avoided by not running explain with this
    // configuration.
    const skipExplainInErrorMessage = TestData.runInsideTransaction && TestData.runningWithShardStepdowns;

    let data = {
        // Number of docs to insert at the beginning.
        nDocs: 200,
        // Batch size of queries to introduce more saving and restoring of states.
        batchSize: 3,
        // The words that can be found in the collection.
        words: ["these", "are", "test", "words"],
        /*
         * Helper function to advance a cursor, and verify that the documents that come out are
         * what we'd expect.
         */
        advanceCursor: function advanceCursor(cursor, verifier) {
            function safeExplain(cursor) {
                try {
                    return cursor.explain();
                } catch (e) {
                    return e;
                }
            }
            // Keep track of the previous doc in case the verifier is trying to verify a sorted
            // query.
            let prevDoc = null;
            let doc = null;
            while (cursor.hasNext()) {
                prevDoc = doc;
                doc = cursor.next();
                assert(
                    verifier(doc, prevDoc),
                    "Verifier failed!\nQuery: " +
                        tojson(cursor._query) +
                        "\n" +
                        (skipExplainInErrorMessage ? "" : "Query plan: " + tojson(safeExplain(cursor))) +
                        "\n" +
                        "Previous doc: " +
                        tojson(prevDoc) +
                        "\n" +
                        "This doc: " +
                        tojson(doc),
                );
            }
            assert.eq(cursor.itcount(), 0);
        },
        /*
         * Many subclasses will want different behavior in the update stage. To change what types
         * of updates happen, they can simply override this function to return which update doc
         * the update state should use for the update query.
         */
        genUpdateDoc: function genUpdateDoc() {
            let newVal = Random.randInt(this.nDocs);
            return {$set: {a: newVal}};
        },
    };

    let states = {
        /*
         * Update a random document from the collection.
         */
        update: function update(db, collName) {
            let id = Random.randInt(this.nDocs);
            let randDoc = db[collName].findOne({_id: id});
            if (randDoc === null) {
                return;
            }
            let updateDoc = this.genUpdateDoc();
            assert.commandWorked(db[collName].update(randDoc, updateDoc));
        },

        /*
         * Remove a random document from the collection, then re-insert one to prevent losing
         * documents.
         */
        remove: function remove(db, collName) {
            let id = Random.randInt(this.nDocs);
            let doc = db[collName].findOne({_id: id});
            if (doc !== null) {
                let res = db[collName].remove({_id: id});
                assert.commandWorked(res);
                if (res.nRemoved > 0) {
                    assert.commandWorked(db[collName].insert(doc));
                }
            }
        },

        /*
         * Issue a query that will potentially yield and resume while documents are being updated.
         * Subclasses will implement this differently
         */
        query: function collScan(db, collName) {
            let nMatches = 100;
            let cursor = db[collName].find({a: {$lt: nMatches}}).batchSize(this.batchSize);
            let collScanVerifier = function collScanVerifier(doc, prevDoc) {
                return doc.a < nMatches;
            };

            this.advanceCursor(cursor, collScanVerifier);
        },
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
    let transitions = {
        update: {update: 0.334, remove: 0.333, query: 0.333},
        remove: {update: 0.333, remove: 0.334, query: 0.333},
        query: {update: 0.333, remove: 0.333, query: 0.334},
    };

    /*
     * Sets up the indices, sets a failpoint and lowers some yielding parameters to encourage
     * more yielding, and inserts the documents to be used.
     */
    function setup(db, collName, cluster) {
        // Lower the following parameters to force even more yields.
        cluster.executeOnMongodNodes(function lowerYieldParams(db) {
            assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 5}));
            assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryExecYieldPeriodMS: 1}));
        });
        // Set up some data to query.
        let N = this.nDocs;
        let bulk = db[collName].initializeUnorderedBulkOp();
        for (let i = 0; i < N; i++) {
            // Give each doc some word of text
            let word = this.words[i % this.words.length];
            bulk.find({_id: i})
                .upsert()
                .updateOne({$set: {a: i, b: N - i, c: i, d: N - i, yield_text: word}});
        }
        assert.commandWorked(bulk.execute());
    }

    /*
     * Reset parameters and disable failpoint.
     */
    function teardown(db, collName, cluster) {
        cluster.executeOnMongodNodes(function resetYieldParams(db) {
            assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 128}));
            assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryExecYieldPeriodMS: 10}));
        });
    }

    return {
        threadCount: 5,
        iterations: 50,
        startState: "update",
        states: states,
        transitions: transitions,
        setup: setup,
        teardown: teardown,
        data: data,
    };
})();
