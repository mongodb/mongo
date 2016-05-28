'use strict';

/**
 * map_reduce_reduce.js
 *
 * Generates some random data and inserts it into a collection. Runs a
 * map-reduce command over the collection that computes the frequency
 * counts of the 'value' field and stores the results in an existing
 * collection.
 *
 * Uses the "reduce" action to combine the results with the contents
 * of the output collection.
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');         // for extendWorkload
load('jstests/concurrency/fsm_workloads/map_reduce_inline.js');  // for $config
load('jstests/concurrency/fsm_workload_helpers/drop_utils.js');  // for dropCollections

var $config = extendWorkload($config, function($config, $super) {

    // Use the workload name as a prefix for the collection name,
    // since the workload name is assumed to be unique.
    var prefix = 'map_reduce_reduce';

    function uniqueCollectionName(prefix, tid) {
        return prefix + tid;
    }

    $config.states.init = function init(db, collName) {
        $super.states.init.apply(this, arguments);

        this.outCollName = uniqueCollectionName(prefix, this.tid);
        assertAlways.commandWorked(db.createCollection(this.outCollName));
    };

    $config.states.mapReduce = function mapReduce(db, collName) {
        var fullName = db[this.outCollName].getFullName();
        assertAlways(db[this.outCollName].exists() !== null,
                     "output collection '" + fullName + "' should exist");

        var options = {finalize: this.finalizer, out: {reduce: this.outCollName}};

        var res = db[collName].mapReduce(this.mapper, this.reducer, options);
        assertAlways.commandWorked(res);
    };

    $config.teardown = function teardown(db, collName, cluster) {
        var pattern = new RegExp('^' + prefix + '\\d+$');
        dropCollections(db, pattern);
    };

    return $config;
});
