/*
 * yield_sort.js (extends yield_sort_merge.js)
 *
 * Intersperse queries which use the SORT stage with updates and deletes of documents they may
 * match.
 * @tags: [
 *   requires_getmore
 * ]
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/yield_sort_merge.js";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    /*
     * Execute a query that will use the SORT stage.
     */
    $config.states.query = function sort(db, collName) {
        var nMatches = 100;
        // Sort on c, since it's not an indexed field.
        var cursor =
            db[collName].find({a: {$lt: nMatches}}).sort({c: -1}).batchSize(this.batchSize);

        var verifier = function sortVerifier(doc, prevDoc) {
            var correctOrder = true;
            if (prevDoc !== null) {
                correctOrder = (doc.c <= prevDoc.c);
            }
            return doc.a < nMatches && correctOrder;
        };

        this.advanceCursor(cursor, verifier);
    };

    $config.data.genUpdateDoc = function genUpdateDoc() {
        var newA = Random.randInt(this.nDocs);
        var newC = Random.randInt(this.nDocs);
        return {$set: {a: newA, c: newC}};
    };

    return $config;
});
