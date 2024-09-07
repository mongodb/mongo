/**
 * distinct_projection.js
 *
 * Runs distinct, with a projection on an indexed field, and verifies the result.
 * The indexed field contains unique values.
 * Each thread operates on a separate collection.
 *
 * @tags: [
 *   # TODO SERVER-13116: distinct isn't sharding aware
 *   assumes_balancer_off,
 * ]
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/query/distinct/distinct.js";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    $config.data.prefix = 'distinct_projection_fsm';

    $config.states.distinct = function distinct(db, collName) {
        var query = {i: {$lt: this.numDocs / 2}};
        assert.eq(this.numDocs / 2, db[this.threadCollName].distinct('i', query).length);
    };

    return $config;
});
