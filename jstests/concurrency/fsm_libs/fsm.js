import {withTxnAndAutoRetry} from "jstests/concurrency/fsm_workload_helpers/auto_retry_transaction.js";
import {ShardTransitionUtil} from "jstests/libs/shard_transition_util.js";
import {TxnUtil} from "jstests/libs/txns/txn_util.js";

export var fsm = (function () {
    const kIsRunningInsideTransaction = Symbol("isRunningInsideTransaction");

    function forceRunningOutsideTransaction(data) {
        if (data[kIsRunningInsideTransaction]) {
            const err = new Error(
                "Intentionally thrown to stop state function from running inside of a" + " multi-statement transaction",
            );
            err.isNotSupported = true;
            throw err;
        }
    }

    // args.data = 'this' object of the state functions
    // args.db = database object
    // args.collName = collection name
    // args.cluster = connection strings for all cluster nodes (see fsm_libs/cluster.js for format)
    // args.passConnectionCache = boolean, whether to pass a connection cache to the workload states
    // args.startState = name of initial state function
    // args.states = state functions of the form
    //               { stateName: function(db, collName) { ... } }
    // args.tid = the thread identifier
    // args.transitions = transitions between state functions of the form
    //                    { stateName: { nextState1: probability,
    //                                   nextState2: ... } }
    // args.iterations = number of iterations to run the FSM for
    // args.errorLatch = the latch that a thread count downs when it errors.
    // args.numThreads = total number of threads running.
    async function runFSM(args) {
        if (TestData.runInsideTransaction) {
            let overridePath = "jstests/libs/override_methods/";
            await import(overridePath + "check_for_operation_not_supported_in_transaction.js");
            await import("jstests/concurrency/fsm_workload_helpers/auto_retry_transaction.js");
            await import("jstests/libs/txns/txn_util.js");
        }
        let currentState = args.startState;

        // We build a cache of connections that can be used in workload states. This cache
        // allows state functions to access arbitrary cluster nodes for verification checks.
        // See fsm_libs/cluster.js for the format of args.cluster.
        let connCache;
        if (args.passConnectionCache) {
            // In order to ensure that all operations performed by a worker thread happen on the
            // same session, we override the "_defaultSession" property of the connections in the
            // cache to be the same as the session underlying 'args.db'.
            const makeNewConnWithExistingSession = function (connStr) {
                // We may fail to connect if the continuous stepdown thread had just terminated
                // or killed a primary. We therefore use the connect() function defined in
                // network_error_and_transaction_override.js to add automatic retries to
                // connections. The override is loaded in worker_thread.js.
                const conn = connect(connStr).getMongo();
                conn._defaultSession = new _DelegatingDriverSession(conn, args.db.getSession());
                return conn;
            };

            const getReplSetName = (conn) => {
                let res;
                assert.soonNoExcept(
                    () => {
                        res = conn.getDB("admin").runCommand({isMaster: 1});
                        return true;
                    },
                    "Failed to establish a connection to the replica set",
                    undefined, // default timeout is 10 mins
                    2 * 1000,
                ); // retry on a 2 second interval

                assert.commandWorked(res);
                assert.eq("string", typeof res.setName, () => `not connected to a replica set: ${tojson(res)}`);
                return res.setName;
            };

            const makeReplSetConnWithExistingSession = (connStrList, replSetName) => {
                let connStr = `mongodb://${connStrList.join(",")}/?appName=tid:${args.tid}&replicaSet=${replSetName}`;
                if (jsTestOptions().shellGRPC) {
                    connStr += "&grpc=false";
                }
                return makeNewConnWithExistingSession(connStr);
            };

            // Config shard conn strings do not use gRPC.
            const kNonGrpcConnStr = (connStr) =>
                jsTestOptions().shellGRPC ? `mongodb://${connStr}/?grpc=false` : `mongodb://${connStr}`;

            connCache = {mongos: [], config: [], shards: {}, rsConns: {config: undefined, shards: {}}};
            connCache.mongos = args.cluster.mongos.map(makeNewConnWithExistingSession);
            connCache.config = args.cluster.config
                .map((connStr) => kNonGrpcConnStr(connStr))
                .map(makeNewConnWithExistingSession);
            connCache.rsConns.config = makeReplSetConnWithExistingSession(
                args.cluster.config,
                getReplSetName(connCache.config[0]),
            );

            // We set _isConfigServer=true on the Mongo connection object so
            // set_read_preference_secondary.js knows to avoid overriding the read preference as the
            // concurrency suite may be running with a 1-node CSRS.
            connCache.rsConns.config._isConfigServer = true;

            let shardNames = Object.keys(args.cluster.shards);

            shardNames.forEach((name) => {
                connCache.shards[name] = args.cluster.shards[name]
                    .map((connStr) => kNonGrpcConnStr(connStr))
                    .map(makeNewConnWithExistingSession);
                connCache.rsConns.shards[name] = makeReplSetConnWithExistingSession(
                    args.cluster.shards[name],
                    getReplSetName(connCache.shards[name][0]),
                );
            });
        }

        for (let i = 0; i < args.iterations; ++i) {
            if (args.errorLatch.getCount() < args.numThreads) {
                break;
            }

            var fn = args.states[currentState];

            assert.eq("function", typeof fn, "states." + currentState + " is not a function");

            if (TestData.runInsideTransaction) {
                try {
                    // We make a deep copy of 'args.data' before running the state function in a
                    // transaction so that if the transaction aborts, then we haven't speculatively
                    // modified the thread-local state.
                    let data;
                    withTxnAndAutoRetry(
                        args.db.getSession(),
                        () => {
                            data = TxnUtil.deepCopyObject({}, args.data);
                            data[kIsRunningInsideTransaction] = true;

                            // The state function is given a Proxy object to the connection cache which
                            // intercepts property accesses (e.g. `connCacheProxy.rsConns`) and causes
                            // the state function to fall back to being called outside of
                            // withTxnAndAutoRetry() as if an operation within the transaction had
                            // failed with OperationNotSupportedInTransaction or InvalidOptions. Usage
                            // of the connection cache isn't compatible with
                            // `TestData.runInsideTransaction === true`. This is because the
                            // withTxnAndAutoRetry() function isn't aware of transactions started
                            // directly on replica set shards or the config server replica set to
                            // automatically commit them and leaked transactions would stall the test
                            // indefinitely.
                            let connCacheProxy;
                            if (connCache !== undefined) {
                                connCacheProxy = new Proxy(connCache, {
                                    get: function (target, prop, receiver) {
                                        forceRunningOutsideTransaction(data);
                                        return target[prop];
                                    },
                                });
                            }

                            fn.call(data, args.db, args.collName, connCacheProxy);
                        },
                        {retryOnKilledSession: args.data.retryOnKilledSession},
                    );
                    delete data[kIsRunningInsideTransaction];
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
            } else if (TestData.shardsAddedRemoved) {
                // Some state functions choose a specific shard for a command and may fail with
                // ShardNotFound if the shard list changes during execution. Retry on the assumption
                // a different shard will be chosen on the retry or soon the config server will
                // become a shard again.
                ShardTransitionUtil.retryOnShardTransitionErrors(() => {
                    fn.call(args.data, args.db, args.collName, connCache);
                });
            } else {
                fn.call(args.data, args.db, args.collName, connCache);
            }

            assert(args.transitions.hasOwnProperty(currentState), `No transitions defined for state: ${currentState}`);
            const nextState = getWeightedRandomChoice(args.transitions[currentState], Random.rand());
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

        let states = Object.keys(doc);
        assert.gt(states.length, 0, "transition must have at least one state to transition to");

        // weights = [ 0.25, 0.5, 0.25 ]
        // => accumulated = [ 0.25, 0.75, 1 ]
        let weights = states.map(function (k) {
            return doc[k];
        });

        let accumulated = [];
        let sum = weights.reduce(function (a, b, i) {
            accumulated[i] = a + b;
            return accumulated[i];
        }, 0);

        // Scale the random value by the sum of the weights
        randVal *= sum; // ~ U[0, sum)

        // Find the state corresponding to randVal
        for (let i = 0; i < accumulated.length; ++i) {
            if (randVal < accumulated[i]) {
                return states[i];
            }
        }
        assert(false, "not reached");
    }

    return {
        forceRunningOutsideTransaction,
        run: runFSM,
        _getWeightedRandomChoice: getWeightedRandomChoice,
    };
})();
