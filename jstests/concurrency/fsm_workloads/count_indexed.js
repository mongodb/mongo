'use strict';

/**
 * count_indexed.js
 *
 * Runs count on an indexed field (using hint), which results in an index scan,
 * and verifies the result.
 * Each thread picks a random 'modulus' in range [5, 10]
 * and a random 'countPerNum' in range [50, 100]
 * and then inserts 'modulus * countPerNum' documents. [250, 1000]
 * Each thread inserts docs into a unique collection.
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');  // for extendWorkload
load('jstests/concurrency/fsm_workloads/count.js');       // for $config

var $config = extendWorkload($config, function($config, $super) {
    $config.data.prefix = 'count_fsm';
    $config.data.shardKey = {tid: 1, i: 1};

    $config.data.getCount = function getCount(db, predicate) {
        var query = Object.extend({tid: this.tid}, predicate);
        return db[this.threadCollName].find(query).hint({tid: 1, i: 1}).count();
    };

    $config.states.init = function init(db, collName) {
        this.threadCollName = this.prefix + '_' + this.tid;
        $super.states.init.apply(this, arguments);
        assertAlways.commandWorked(db[this.threadCollName].ensureIndex({tid: 1, i: 1}));
    };

    return $config;
});
