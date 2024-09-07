/**
 * count_indexed.js
 *
 * Runs count on an indexed field (using hint), which results in an index scan,
 * and verifies the result.
 * Each thread picks a random 'modulus' in range [5, 10]
 * and a random 'countPerNum' in range [50, 100]
 * and then inserts 'modulus * countPerNum' documents. [250, 1000]
 * Each thread inserts docs into a unique collection.
 *
 * TODO SERVER-91708: An index build queued to start but not added to the list of active index
 * builds yet will allow a moveCollection to proceed and the index build will fail to build
 * on account of the collection being dropped on the donor.
 * @tags: [assumes_balancer_off]
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/query/count/count.js";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    $config.data.prefix = 'count_fsm';
    $config.data.shardKey = {tid: 1, i: 1};

    $config.data.getCount = function getCount(db, predicate) {
        var query = Object.extend({tid: this.tid}, predicate);
        return db[this.threadCollName].find(query).hint({tid: 1, i: 1}).count();
    };

    $config.states.init = function init(db, collName) {
        this.threadCollName = this.prefix + '_' + this.tid;
        $super.states.init.apply(this, arguments);
        assert.commandWorked(db[this.threadCollName].createIndex({tid: 1, i: 1}));
    };

    return $config;
});
