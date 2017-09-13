'use strict';

/**
 * fsm_example.js
 *
 * Includes documentation of each property on $config.
 * Serves as a template for new workloads.
 */
var $config = (function() {

    // 'data' is passed (copied) to each of the worker threads.
    var data = {};

    // 'states' are the different functions callable by a worker
    // thread. The 'this' argument of any exposed function is
    // bound as '$config.data'.
    var states = {
        init: function init(db, collName) {
            this.start = 10 * this.tid;
        },

        scanGT: function scanGT(db, collName) {
            db[collName].find({_id: {$gt: this.start}}).itcount();
        },

        scanLTE: function scanLTE(db, collName) {
            db[collName].find({_id: {$lte: this.start}}).itcount();
        },
    };

    // 'transitions' defines how the FSM should proceed from its
    // current state to the next state. The value associated with a
    // particular state represents the likelihood of that transition.
    //
    // For example, 'init: { scanGT: 0.5, scanLTE: 0.5 }' means that
    // the worker thread will transition from the 'init' state
    //   to the 'scanGT'  state with probability 0.5, and
    //   to the 'scanLTE' state with probability 0.5.
    //
    // All state functions should appear as keys within 'transitions'.
    var transitions = {
        init: {scanGT: 0.5, scanLTE: 0.5},
        scanGT: {scanGT: 0.8, scanLTE: 0.2},
        scanLTE: {scanGT: 0.2, scanLTE: 0.8}
    };

    // 'setup' is run once by the parent thread after the cluster has
    // been initialized, but before the worker threads have been spawned.
    // The 'this' argument is bound as '$config.data'. 'cluster' is provided
    // to allow execution against all mongos and mongod nodes.
    function setup(db, collName, cluster) {
        // Workloads should NOT drop the collection db[collName], as
        // doing so is handled by runner.js before 'setup' is called.
        for (var i = 0; i < 1000; ++i) {
            db[collName].insert({_id: i});
        }

        cluster.executeOnMongodNodes(function(db) {
            printjson(db.serverCmdLineOpts());
        });

        cluster.executeOnMongosNodes(function(db) {
            printjson(db.serverCmdLineOpts());
        });
    }

    // 'teardown' is run once by the parent thread before the cluster
    // is destroyed, but after the worker threads have been reaped.
    // The 'this' argument is bound as '$config.data'. 'cluster' is provided
    // to allow execution against all mongos and mongod nodes.
    function teardown(db, collName, cluster) {
    }

    return {
        threadCount: 5,
        iterations: 10,
        startState: 'init',  // optional, default 'init'
        states: states,
        transitions: transitions,
        setup: setup,        // optional, default empty function
        teardown: teardown,  // optional, default empty function
        data: data           // optional, default empty object
    };

})();
