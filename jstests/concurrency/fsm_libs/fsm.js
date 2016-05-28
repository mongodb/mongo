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
        var currentState = args.startState;

        // We build a cache of connections that can be used in workload states. This cache
        // allows state functions to access arbitrary cluster nodes for verification checks.
        // See fsm_libs/cluster.js for the format of args.cluster.
        var connCache;
        if (args.passConnectionCache) {
            connCache = {mongos: [], config: [], shards: {}};
            connCache.mongos = args.cluster.mongos.map(connStr => new Mongo(connStr));
            connCache.config = args.cluster.config.map(connStr => new Mongo(connStr));

            var shardNames = Object.keys(args.cluster.shards);

            shardNames.forEach(name => (connCache.shards[name] = args.cluster.shards[name].map(
                                            connStr => new Mongo(connStr))));
        }

        for (var i = 0; i < args.iterations; ++i) {
            var fn = args.states[currentState];
            assert.eq('function', typeof fn, 'states.' + currentState + ' is not a function');
            fn.call(args.data, args.db, args.collName, connCache);
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
