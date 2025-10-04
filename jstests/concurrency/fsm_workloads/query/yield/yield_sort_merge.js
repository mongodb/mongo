/*
 * yield_sort_merge.js (extends yield_fetch.js)
 *
 * Intersperse queries which use the SORT_MERGE stage with updates and deletes of documents they
 * may match.
 *
 * Other workloads that need an index { a: 1, b: 1 } can extend this.
 * @tags: [
 *   requires_getmore,
 *   # This test relies on query commands returning specific batch-sized responses.
 *   assumes_no_implicit_cursor_exhaustion,
 * ]
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/query/yield/yield.js";

export const $config = extendWorkload($baseConfig, function ($config, $super) {
    /*
     * Execute a query that will use the SORT_MERGE stage.
     */
    $config.states.query = function sortMerge(db, collName) {
        let nMatches = 50; // Don't push this too high, or SORT_MERGE stage won't be selected.

        // Build an array [0, nMatches).
        let matches = [];
        for (let i = 0; i < nMatches; i++) {
            matches.push(i);
        }

        let cursor = db[collName]
            .find({a: {$in: matches}})
            .sort({b: -1})
            .batchSize(this.batchSize);

        let verifier = function sortMergeVerifier(doc, prevDoc) {
            let correctOrder = true;
            if (prevDoc !== null) {
                correctOrder = doc.b <= prevDoc.b;
            }
            return doc.a < nMatches && correctOrder;
        };

        this.advanceCursor(cursor, verifier);
    };

    $config.data.genUpdateDoc = function genUpdateDoc() {
        let newA = Random.randInt(this.nDocs);
        let newB = Random.randInt(this.nDocs);
        return {$set: {a: newA, b: newB}};
    };

    $config.setup = function setup(db, collName, cluster) {
        $super.setup.apply(this, arguments);

        assert.commandWorked(db[collName].createIndex({a: 1, b: 1}));
    };

    return $config;
});
