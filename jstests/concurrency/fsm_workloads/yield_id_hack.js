'use strict';

/*
 * yield_id_hack.js (extends yield.js)
 *
 * Intersperse queries which use the ID_HACK stage with updates and deletes of documents they may
 * match.
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');  // for extendWorkload
load('jstests/concurrency/fsm_workloads/yield.js');       // for $config

var $config = extendWorkload($config, function($config, $super) {

    /*
     * Issue a query that will use the ID_HACK stage. This cannot be
     * batched, so issue a
     * number of them to increase the chances of yielding between
     * getting the key and looking
     * up its value.
     */
    $config.states.query = function idHack(db, collName) {
        var nQueries = 100;
        for (var i = 0; i < nQueries; i++) {
            assertAlways.lte(db[collName].find({_id: i}).itcount(), 1);
            var res = db[collName].findOne({_id: i});
            if (res !== null) {
                assertAlways.eq(i, res._id);
            }
        }
    };

    return $config;
});
