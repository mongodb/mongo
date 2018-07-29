'use strict';

/**
 * map_reduce_replace_remove.js
 *
 * Generates some random data and inserts it into a collection. Runs a map-reduce command over the
 * collection that computes the frequency counts of the 'value' field and stores the results in an
 * existing collection. Some of the random data from the source collection is removed while the
 * map-reduce operations are running to verify the cursor state is saved and restored correctly on
 * yields.
 *
 * This workload was designed to reproduce SERVER-15539.
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');          // for extendWorkload
load('jstests/concurrency/fsm_workloads/map_reduce_replace.js');  // for $config

var $config = extendWorkload($config, function($config, $super) {

    $config.states.remove = function remove(db, collName) {
        for (var i = 0; i < 20; ++i) {
            var res = db[collName].remove({value: {$gte: Random.randInt(this.numDocs / 10)}},
                                          {justOne: true});
            assertAlways.writeOK(res);
            assertAlways.lte(0, res.nRemoved, tojson(res));
        }
    };

    $config.transitions = {
        init: {mapReduce: 0.5, remove: 0.5},
        mapReduce: {mapReduce: 0.5, remove: 0.5},
        remove: {mapReduce: 0.5, remove: 0.5}
    };

    return $config;
});
