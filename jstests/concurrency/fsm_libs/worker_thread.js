'use strict';

load('jstests/concurrency/fsm_libs/assert.js');
load('jstests/concurrency/fsm_libs/cluster.js'); // for Cluster.isStandalone
load('jstests/concurrency/fsm_libs/parse_config.js'); // for parseConfig

var workerThread = (function() {

    // workloads = list of workload filenames
    // args.data = 'this' parameter passed to the FSM state functions
    // args.data.tid = the thread identifier
    // args.host = the address to make a new connection to
    // args.latch = CountDownLatch instance for starting all threads
    // args.dbName = the database name
    // args.collName = the collection name
    // args.clusterOptions = the configuration of the cluster
    // args.seed = seed for the random number generator
    // args.globalAssertLevel = the global assertion level to use
    // run = callback that takes a map of workloads to their associated $config
    function main(workloads, args, run) {
        var myDB;
        var configs = {};

        globalAssertLevel = args.globalAssertLevel;

        try {
            if (Cluster.isStandalone(args.clusterOptions)) {
                myDB = db.getSiblingDB(args.dbName);
            } else {
                // The implicit database connection created within the thread's scope
                // is unneeded, so forcibly clean it up
                db = null;
                gc();

                myDB = new Mongo(args.host).getDB(args.dbName);
            }

            workloads.forEach(function(workload) {
                load(workload); // for $config
                var config = parseConfig($config); // to normalize

                // Copy any modifications that were made to $config.data
                // during the setup function of the workload
                var data = Object.extend({}, args.data, true);
                data = Object.extend(data, config.data, true);

                configs[workload] = {
                    data: data,
                    db: myDB,
                    collName: args.collName,
                    startState: config.startState,
                    states: config.states,
                    transitions: config.transitions,
                    iterations: config.iterations
                };
            });

            args.latch.countDown();

            // Converts any exceptions to a return status. In order for the
            // parent thread to call countDown() on our behalf, we must throw
            // an exception. Nothing prior to (and including) args.latch.countDown()
            // should be wrapped in a try/catch statement.
            try {
                args.latch.await(); // wait for all threads to start

                Random.setRandomSeed(args.seed);
                run(configs);
                return { ok: 1 };
            } catch(e) {
                return { ok: 0, err: e.toString(), stack: e.stack };
            }
        } finally {
            // Avoid retention of connection object
            configs = null;
            myDB = null;
            gc();
        }
    }

    return {
        main: main
    };

})();
