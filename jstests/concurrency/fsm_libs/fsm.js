'use strict';

var fsm = (function() {
    // args.data = 'this' object of the state functions
    // args.db = database object
    // args.collName = collection name
    // args.cluster = connection strings for all cluster nodes (see fsm_libs/cluster.js for format)
    // args.passConnectionCache = boolean, whether to pass a connection cache to the workload states
    // args.startState = name of initial state function
    // args.states = state functions of the form
    //               { stateName: function(db, collName) { ... } }
    // args.transitions = transitions between state functions of the form
    //                    { stateName: { nextState1: probability,
    //                                   nextState2: ... } }
    // args.iterations = number of iterations to run the FSM for
    function runFSM(args) {
        if (TestData.runInsideTransaction) {
            let overridePath = "jstests/libs/override_methods/";
            load(overridePath + "check_for_operation_not_supported_in_transaction.js");
            load("jstests/concurrency/fsm_workload_helpers/auto_retry_transaction.js");
            load("jstests/libs/transactions_util.js");
        }
        var currentState = args.startState;

        // We build a cache of connections that can be used in workload states. This cache
        // allows state functions to access arbitrary cluster nodes for verification checks.
        // See fsm_libs/cluster.js for the format of args.cluster.
        var connCache;
        if (args.passConnectionCache) {
            // In order to ensure that all operations performed by a worker thread happen on the
            // same session, we override the "_defaultSession" property of the connections in the
            // cache to be the same as the session underlying 'args.db'.
            const makeNewConnWithExistingSession = function(connStr) {
                // We may fail to connect if the continuous stepdown thread had just terminated
                // or killed a primary. We therefore use the connect() function defined in
                // network_error_and_transaction_override.js to add automatic retries to
                // connections. The override is loaded in worker_thread.js.
                const conn = connect(connStr).getMongo();
                conn._defaultSession = new _DelegatingDriverSession(conn, args.db.getSession());
                return conn;
            };

            connCache = {mongos: [], config: [], shards: {}};
            connCache.mongos = args.cluster.mongos.map(makeNewConnWithExistingSession);
            connCache.config = args.cluster.config.map(makeNewConnWithExistingSession);

            var shardNames = Object.keys(args.cluster.shards);

            shardNames.forEach(name => (connCache.shards[name] = args.cluster.shards[name].map(
                                            makeNewConnWithExistingSession)));
        }

        for (var i = 0; i < args.iterations; ++i) {
            var fn = args.states[currentState];

            assert.eq('function', typeof fn, 'states.' + currentState + ' is not a function');

            if (TestData.runInsideTransaction) {
                try {
                    // We make a deep copy of 'args.data' before running the state function in a
                    // transaction so that if the transaction aborts, then we haven't speculatively
                    // modified the thread-local state.
                    let data;
                    withTxnAndAutoRetry(args.db.getSession(), () => {
                        data = TransactionsUtil.deepCopyObject({}, args.data);
                        fn.call(data, args.db, args.collName, connCache);
                    });
                    args.data = data;
                } catch (e) {
                    // Retry state functions that threw OperationNotSupportedInTransaction or
                    // InvalidOptions errors outside of a transaction. Rethrow any other error.
                    // e.isNotSupported added by check_for_operation_not_supported_in_transaction.js
                    if (!e.isNotSupported) {
                        throw e;
                    }

                    fn.call(args.data, args.db, args.collName, connCache);
                }
            } else {
                fn.call(args.data, args.db, args.collName, connCache);
            }

            var nextState = getWeightedRandomChoice(args.transitions[currentState], Random.rand());
            currentState = nextState;
        }

        // Null out the workload connection cache and perform garbage collection to clean up,
        // i.e., close, the open connections.
        if (args.passConnectionCache) {
            connCache = null;
            gc();
        }
    }

    // doc = document of the form
    //       { nextState1: probability, nextState2: ... }
    // randVal = a value on the interval [0, 1)
    // returns a state, weighted by its probability,
    //    assuming randVal was chosen randomly by the caller
    function getWeightedRandomChoice(doc, randVal) {
        assert.gte(randVal, 0);
        assert.lt(randVal, 1);

        var states = Object.keys(doc);
        assert.gt(states.length, 0, 'transition must have at least one state to transition to');

        // weights = [ 0.25, 0.5, 0.25 ]
        // => accumulated = [ 0.25, 0.75, 1 ]
        var weights = states.map(function(k) {
            return doc[k];
        });

        var accumulated = [];
        var sum = weights.reduce(function(a, b, i) {
            accumulated[i] = a + b;
            return accumulated[i];
        }, 0);

        // Scale the random value by the sum of the weights
        randVal *= sum;  // ~ U[0, sum)

        // Find the state corresponding to randVal
        for (var i = 0; i < accumulated.length; ++i) {
            if (randVal < accumulated[i]) {
                return states[i];
            }
        }
        assert(false, 'not reached');
    }

    return {run: runFSM, _getWeightedRandomChoice: getWeightedRandomChoice};
})();
