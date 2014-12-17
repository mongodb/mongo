var workerThread = (function() {

    // workloads = list of workload filenames
    // args.tid = the thread identifier
    // args.latch = the CountDownLatch instance for starting all threads
    // args.dbName = the database name
    // args.collName = the collection name
    // args.clusterOptions = the configuration of the cluster
    // args.seed = seed for the random number generator
    // args.globalAssertLevel = the global assertion level to use
    // run = callback that takes a map of workloads to their associated $config
    function main(workloads, args, run) {
        var myDB;
        var configs = {};

        try {
            load('jstests/parallel/fsm_libs/assert.js');
            globalAssertLevel = args.globalAssertLevel;

            if (args.clusterOptions.addr) {
                // We won't use the implicit db connection created within the thread's scope, so
                // forcibly clean it up before creating a new connection.
                db = null;
                gc();

                myDB = new Mongo(args.clusterOptions.addr).getDB(args.dbName);
            } else {
                myDB = db.getSiblingDB(args.dbName);
            }

            load('jstests/parallel/fsm_libs/runner.js'); // for parseConfig
            workloads.forEach(function(workload) {
                load(workload);
                var config = parseConfig($config); // to normalize
                config.data.tid = args.tid;
                configs[workload] = {
                    data: config.data,
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
