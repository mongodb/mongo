'use strict';

/**
 * distinct_projection.js
 *
 * Runs distinct, with a projection on an indexed field, and verifies the result.
 * The indexed field contains unique values.
 * Each thread operates on a separate collection.
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');  // for extendWorkload
load('jstests/concurrency/fsm_workloads/distinct.js');    // for $config

var $config = extendWorkload($config, function($config, $super) {
    $config.data.prefix = 'distinct_projection_fsm';

    $config.states.distinct = function distinct(db, collName) {
        var query = {i: {$lt: this.numDocs / 2}};
        assertWhenOwnColl.eq(this.numDocs / 2, db[this.threadCollName].distinct('i', query).length);
    };

    return $config;
});
