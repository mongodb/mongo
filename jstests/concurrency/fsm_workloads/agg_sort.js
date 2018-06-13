'use strict';

/**
 * agg_sort.js
 *
 * Runs an aggregation with a $match that returns half the documents followed
 * by a $sort on a field containing a random float.
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');  // for extendWorkload
load('jstests/concurrency/fsm_workloads/agg_base.js');    // for $config

var $config = extendWorkload($config, function($config, $super) {

    $config.data.getOutputCollPrefix = function getOutputCollPrefix(collName) {
        return collName + '_out_agg_sort_';
    };

    $config.states.query = function query(db, collName) {
        var otherCollName = this.getOutputCollPrefix(collName) + this.tid;
        var cursor = db[collName].aggregate(
            [{$match: {flag: true}}, {$sort: {rand: 1}}, {$out: otherCollName}]);
        assertAlways.eq(0, cursor.itcount());
        assertWhenOwnColl.eq(db[collName].find().itcount() / 2, db[otherCollName].find().itcount());
    };

    return $config;
});
