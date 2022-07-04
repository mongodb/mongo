'use strict';

/**
 * map_reduce_replace_nonexistent.js
 *
 * Generates some random data and inserts it into a collection. Runs a
 * map-reduce command over the collection that computes the frequency
 * counts of the 'value' field and stores the results in a new collection.
 *
 * Uses the "replace" action to write the results to a nonexistent
 * output collection.
 * @tags: [
 *   # mapReduce does not support afterClusterTime.
 *   does_not_support_causal_consistency
 * ]
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');         // for extendWorkload
load('jstests/concurrency/fsm_workloads/map_reduce_inline.js');  // for $config

var $config = extendWorkload($config, function($config, $super) {
    // Use the workload name as a prefix for the collection name,
    // since the workload name is assumed to be unique.
    $config.data.prefix = 'map_reduce_replace_nonexistent';

    function uniqueCollectionName(prefix, tid) {
        return prefix + tid;
    }

    $config.states.mapReduce = function mapReduce(db, collName) {
        var outCollName = uniqueCollectionName(this.prefix, this.tid);
        db[outCollName].drop();

        var options = {
            finalize: this.finalizer,
            out: {replace: outCollName},
            query: {key: {$exists: true}, value: {$exists: true}}
        };

        var res = db[collName].mapReduce(this.mapper, this.reducer, options);
        assertAlways.commandWorked(res);
        assertAlways(db[outCollName].drop());
    };

    return $config;
});
