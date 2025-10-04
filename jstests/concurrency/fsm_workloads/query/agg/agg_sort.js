/**
 * agg_sort.js
 *
 * Runs an aggregation with a $match that returns half the documents followed
 * by a $sort on a field containing a random float.
 * @tags: [
 *   # Uses $out, which is non-retryable.
 *   requires_non_retryable_writes,
 *   requires_getmore,
 *   uses_getmore_outside_of_transaction,
 * ]
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/query/agg/agg_base.js";

export const $config = extendWorkload($baseConfig, function ($config, $super) {
    $config.data.getOutputCollPrefix = function getOutputCollPrefix(collName) {
        return collName + "_out_agg_sort_";
    };

    $config.states.query = function query(db, collName) {
        let otherCollName = this.getOutputCollPrefix(collName) + this.tid;
        let cursor = db[collName].aggregate([{$match: {flag: true}}, {$sort: {rand: 1}}, {$out: otherCollName}]);
        assert.eq(0, cursor.itcount());
        assert.eq(db[collName].find().itcount() / 2, db[otherCollName].find().itcount());
    };

    return $config;
});
