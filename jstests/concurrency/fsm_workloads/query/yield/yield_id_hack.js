/*
 * yield_id_hack.js (extends yield.js)
 *
 * Intersperse queries which use the ID_HACK stage with updates and deletes of documents they may
 * match.
 * @tags: [
 *   # Runs a multi: true delete which is non-retryable.
 *   requires_non_retryable_writes
 * ]
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/query/yield/yield.js";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    /*
     * Issue a query that will use the ID_HACK stage. This cannot be
     * batched, so issue a
     * number of them to increase the chances of yielding between
     * getting the key and looking
     * up its value.
     */
    $config.states.query = function idHack(db, collName) {
        var nQueries = 100;
        for (var i = 0; i < nQueries; i++) {
            assert.lte(db[collName].find({_id: i}).itcount(), 1);
            var res = db[collName].findOne({_id: i});
            if (res !== null) {
                assert.eq(i, res._id);
            }
        }
    };

    return $config;
});
