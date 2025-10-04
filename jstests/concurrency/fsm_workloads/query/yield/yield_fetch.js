/*
 * yield_fetch.js (extends yield_rooted_or.js)
 *
 * Intersperse queries which use the FETCH stage with updates and deletes of documents they may
 * match.
 * @tags: [
 *   requires_getmore,
 *   # This test relies on query commands returning specific batch-sized responses.
 *   assumes_no_implicit_cursor_exhaustion,
 * ]
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/query/yield/yield_rooted_or.js";

export const $config = extendWorkload($baseConfig, function ($config, $super) {
    /*
     * Issue a query that will use the FETCH stage.
     */
    $config.states.query = function fetch(db, collName) {
        let nMatches = 100;

        let cursor = db[collName].find({c: {$lt: nMatches}}).batchSize(this.batchSize);

        let verifier = function fetchVerifier(doc, prevDoc) {
            return doc.c < nMatches;
        };

        this.advanceCursor(cursor, verifier);
    };

    return $config;
});
