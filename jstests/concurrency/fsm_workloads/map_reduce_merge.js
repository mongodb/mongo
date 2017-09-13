'use strict';

/**
 * map_reduce_merge.js
 *
 * Generates some random data and inserts it into a collection. Runs a
 * map-reduce command over the collection that computes the frequency
 * counts of the 'value' field and stores the results in an existing
 * collection on a separate database.
 *
 * Uses the "merge" action to combine the results with the contents
 * of the output collection.
 *
 * Writes the results of each thread to the same collection.
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');         // for extendWorkload
load('jstests/concurrency/fsm_workloads/map_reduce_inline.js');  // for $config

var $config = extendWorkload($config, function($config, $super) {

    // Use the workload name as the database name,
    // since the workload name is assumed to be unique.
    var uniqueDBName = 'map_reduce_merge';

    $config.states.init = function init(db, collName) {
        $super.states.init.apply(this, arguments);

        this.outDBName = db.getName() + uniqueDBName;
    };

    $config.states.mapReduce = function mapReduce(db, collName) {
        var outDB = db.getSiblingDB(this.outDBName);
        var fullName = outDB[collName].getFullName();
        assertAlways(outDB[collName].exists() !== null,
                     "output collection '" + fullName + "' should exist");

        // Have all threads combine their results into the same collection
        var options = {finalize: this.finalizer, out: {merge: collName, db: this.outDBName}};

        var res = db[collName].mapReduce(this.mapper, this.reducer, options);
        assertAlways.commandWorked(res);
    };

    $config.setup = function setup(db, collName, cluster) {
        $super.setup.apply(this, arguments);

        var outDB = db.getSiblingDB(db.getName() + uniqueDBName);
        assertAlways.commandWorked(outDB.createCollection(collName));
    };

    $config.teardown = function teardown(db, collName, cluster) {
        var outDB = db.getSiblingDB(db.getName() + uniqueDBName);
        var res = outDB.dropDatabase();
        assertAlways.commandWorked(res);
        assertAlways.eq(db.getName() + uniqueDBName, res.dropped);
    };

    return $config;
});
