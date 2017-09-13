'use strict';

/*
 * yield_sort_merge.js (extends yield_fetch.js)
 *
 * Intersperse queries which use the SORT_MERGE stage with updates and deletes of documents they
 * may match.
 * Other workloads that need an index { a: 1, b: 1 } can extend this
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');  // for extendWorkload
load('jstests/concurrency/fsm_workloads/yield.js');       // for $config

var $config = extendWorkload($config, function($config, $super) {

    /*
     * Execute a query that will use the SORT_MERGE stage.
     */
    $config.states.query = function sortMerge(db, collName) {
        var nMatches = 50;  // Don't push this too high, or SORT_MERGE stage won't be selected.

        // Build an array [0, nMatches).
        var matches = [];
        for (var i = 0; i < nMatches; i++) {
            matches.push(i);
        }

        var cursor = db[collName].find({a: {$in: matches}}).sort({b: -1}).batchSize(this.batchSize);

        var verifier = function sortMergeVerifier(doc, prevDoc) {
            var correctOrder = true;
            if (prevDoc !== null) {
                correctOrder = (doc.b <= prevDoc.b);
            }
            return doc.a < nMatches && correctOrder;
        };

        this.advanceCursor(cursor, verifier);
    };

    $config.data.genUpdateDoc = function genUpdateDoc() {
        var newA = Random.randInt(this.nDocs);
        var newB = Random.randInt(this.nDocs);
        return {$set: {a: newA, b: newB}};
    };

    $config.setup = function setup(db, collName, cluster) {
        $super.setup.apply(this, arguments);

        assertAlways.commandWorked(db[collName].ensureIndex({a: 1, b: 1}));
    };

    return $config;
});
