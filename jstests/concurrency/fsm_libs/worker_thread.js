'use strict';

load('jstests/concurrency/fsm_libs/assert.js');
load('jstests/concurrency/fsm_libs/cluster.js');       // for Cluster.isStandalone
load('jstests/concurrency/fsm_libs/parse_config.js');  // for parseConfig

var workerThread = (function() {

    // workloads = list of workload filenames
    // args.tid = the thread identifier
    // args.data = map of workload -> 'this' parameter passed to the FSM state functions
    // args.host = the address to make a new connection to
    // args.latch = CountDownLatch instance for starting all threads
    // args.dbName = the database name
    // args.collName = the collection name
    // args.cluster = connection strings for all cluster nodes (see cluster.js for format)
    // args.clusterOptions = the configuration of the cluster
    // args.seed = seed for the random number generator
    // args.globalAssertLevel = the global assertion level to use
    // args.errorLatch = CountDownLatch instance that threads count down when they error
    // run = callback that takes a map of workloads to their associated $config
    function main(workloads, args, run) {
        var myDB;
        var configs = {};

        globalAssertLevel = args.globalAssertLevel;

        try {
            if (Cluster.isStandalone(args.clusterOptions)) {
                myDB = db.getSiblingDB(args.dbName);
            } else {
                if (typeof db !== 'undefined') {
                    // The implicit database connection created within the thread's scope
                    // is unneeded, so forcibly clean it up.
                    db = null;
                    gc();
                }

                myDB = new Mongo(args.host).getDB(args.dbName);
            }

            workloads.forEach(function(workload) {
                load(workload);                     // for $config
                var config = parseConfig($config);  // to normalize

                // Copy any modifications that were made to $config.data
                // during the setup function of the workload (see caveat
                // below).

                // XXX: Changing the order of extend calls causes problems
                // for workloads that reference $super.
                // Suppose you have workloads A and B, where workload B extends
                // workload A. The $config.data of workload B can define a
                // function that closes over the $config object of workload A
                // (known as $super to workload B). This reference is lost when
                // the config object is serialized to BSON, which results in
                // undefined variables in the derived workload.
                var data = Object.extend({}, args.data[workload], true);
                data = Object.extend(data, config.data, true);

                // Object.extend() defines all properties added to the destination object as
                // configurable, enumerable, and writable. To prevent workloads from changing
                // the iterations and threadCount properties in their state functions, we redefine
                // them here as non-configurable, non-enumerable, and non-writable.
                Object.defineProperties(data, {
                    'iterations': {
                        configurable: false,
                        enumerable: false,
                        writable: false,
                        value: data.iterations
                    },
                    'threadCount': {
                        configurable: false,
                        enumerable: false,
                        writable: false,
                        value: data.threadCount
                    }
                });

                data.tid = args.tid;
                configs[workload] = {
                    data: data,
                    db: myDB,
                    collName: args.collName,
                    cluster: args.cluster,
                    iterations: data.iterations,
                    passConnectionCache: config.passConnectionCache,
                    startState: config.startState,
                    states: config.states,
                    transitions: config.transitions
                };
            });

            args.latch.countDown();

            // Converts any exceptions to a return status. In order for the
            // parent thread to call countDown() on our behalf, we must throw
            // an exception. Nothing prior to (and including) args.latch.countDown()
            // should be wrapped in a try/catch statement.
            try {
                args.latch.await();  // wait for all threads to start

                Random.setRandomSeed(args.seed);
                run(configs);
                return {ok: 1};
            } catch (e) {
                args.errorLatch.countDown();
                return {
                    ok: 0,
                    err: e.toString(),
                    stack: e.stack,
                    tid: args.tid,
                    workloads: workloads,
                };
            }
        } finally {
            // Avoid retention of connection object
            configs = null;
            myDB = null;
            gc();
        }
    }

    return {main: main};

})();
