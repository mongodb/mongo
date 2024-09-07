/**
 * agg_sort.js
 *
 * Runs an aggregation with a $match that returns half the documents followed
 * by a $sort on a field containing a random float.
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/query/agg/agg_base.js";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    $config.data.getOutputCollPrefix = function getOutputCollPrefix(collName) {
        return collName + '_out_agg_sort_';
    };

    $config.states.query = function query(db, collName) {
        var otherCollName = this.getOutputCollPrefix(collName) + this.tid;
        var cursor = db[collName].aggregate(
            [{$match: {flag: true}}, {$sort: {rand: 1}}, {$out: otherCollName}]);
        assert.eq(0, cursor.itcount());
        assert.eq(db[collName].find().itcount() / 2, db[otherCollName].find().itcount());
    };

    return $config;
});
