/**
 * agg_match.js
 *
 * Runs an aggregation with a $match that returns half the documents.
 * @tags: [
 *   # SERVER-33753, '.count() without a predicate can be wrong on sharded
 *   # collections'. This bug is problematic for these workloads because they assert on count()
 *   # values
 *   assumes_unsharded_collection,
 *   requires_getmore,
 *   uses_getmore_outside_of_transaction,
 * ]
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/query/agg/agg_base.js";

export const $config = extendWorkload($baseConfig, function ($config, $super) {
    $config.states.query = function query(db, collName) {
        let res = db[collName].aggregate([{$match: {flag: true}}, {$count: "count"}]).toArray();
        // NOTE: This relies on the fast-path for .count().
        // NOTE: There's a bug, SERVER-33753, where "fast" .count() is wrong on sharded
        // collections, so we denylisted this test for sharded clusters.
        assert.eq(db[collName].count() / 2, res[0].count);
    };

    return $config;
});
