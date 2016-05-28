'use strict';

/**
 * map_reduce_merge_nonatomic.js
 *
 * Generates some random data and inserts it into a collection. Runs a
 * map-reduce command over the collection that computes the frequency
 * counts of the 'value' field and stores the results in an existing
 * collection on a separate database.
 *
 * Uses the "merge" action to combine the results with the contents
 * of the output collection.
 *
 * Specifies nonAtomic=true.
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');         // for extendWorkload
load('jstests/concurrency/fsm_workloads/map_reduce_inline.js');  // for $config
load('jstests/concurrency/fsm_workload_helpers/drop_utils.js');  // for dropDatabases

var $config = extendWorkload($config, function($config, $super) {

    // Use the workload name as a prefix for the database name,
    // since the workload name is assumed to be unique.
    var prefix = 'map_reduce_merge_nonatomic';

    function uniqueDBName(prefix, tid) {
        return prefix + tid;
    }

    $config.states.init = function init(db, collName) {
        $super.states.init.apply(this, arguments);

        this.outDBName = db.getName() + uniqueDBName(prefix, this.tid);
        var outDB = db.getSiblingDB(this.outDBName);
        assertAlways.commandWorked(outDB.createCollection(collName));
    };

    $config.states.mapReduce = function mapReduce(db, collName) {
        var outDB = db.getSiblingDB(this.outDBName);
        var fullName = outDB[collName].getFullName();
        assertAlways(outDB[collName].exists() !== null,
                     "output collection '" + fullName + "' should exist");

        var options = {
            finalize: this.finalizer,
            out: {merge: collName, db: this.outDBName, nonAtomic: true}
        };

        var res = db[collName].mapReduce(this.mapper, this.reducer, options);
        assertAlways.commandWorked(res);
    };

    $config.teardown = function teardown(db, collName, cluster) {
        var pattern = new RegExp('^' + db.getName() + prefix + '\\d+$');
        dropDatabases(db, pattern);
    };

    return $config;
});
