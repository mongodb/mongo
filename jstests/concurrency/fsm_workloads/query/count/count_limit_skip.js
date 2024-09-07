/**
 * count_limit_skip.js
 *
 * Runs count with skip, limit, and a query (without using hint)
 * resulting in a collection scan and then verifies the result.
 * Each thread picks a random 'modulus' in range [5, 10]
 * and a random 'countPerNum' in range [50, 100]
 * and then inserts 'modulus * countPerNum' documents. [250, 1000]
 * Each thread inserts docs into a unique collection.
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {isMongos} from "jstests/concurrency/fsm_workload_helpers/server_types.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/query/count/count.js";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    $config.data.prefix = 'count_fsm_q_l_s';

    $config.data.getCount = function getCount(db, predicate) {
        var query = Object.extend({tid: this.tid}, predicate);
        return db[this.threadCollName].find(query).skip(this.countPerNum - 1).limit(10).count(true);
    };

    $config.states.init = function init(db, collName) {
        this.threadCollName = this.prefix + '_' + this.tid;

        $super.states.init.apply(this, arguments);
    };

    $config.states.count = function count(db, collName) {
        if (!isMongos(db)) {
            // SERVER-33753: count() without a predicate can be wrong on sharded clusters.
            assert.eq(this.getCount(db),
                      // having done 'skip(this.countPerNum - 1).limit(10)'
                      10);
        }

        var num = Random.randInt(this.modulus);
        assert.eq(this.getCount(db, {i: num}),
                  // having done 'skip(this.countPerNum - 1).limit(10)'
                  1);
    };

    return $config;
});
