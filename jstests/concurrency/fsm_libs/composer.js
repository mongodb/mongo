load('jstests/concurrency/fsm_libs/fsm.js');

var composer = (function() {

    function runCombinedFSM(workloads, configs, mixProb) {
        // TODO: what if a workload depends on iterations?
        var iterations = 100;

        assert.eq(
            AssertLevel.ALWAYS, globalAssertLevel, 'global assertion level is not set as ALWAYS');

        var currentWorkload = getRandomElem(workloads, Random.rand());
        var currentState = configs[currentWorkload].startState;

        var myDB, collName;
        var first = true;
        workloads.forEach(function(workload) {
            var args = configs[workload];
            if (!first) {
                assert.eq(myDB, args.db, 'expected all workloads to use same database');
                assert.eq(collName, args.collName, 'expected all workloads to use same collection');
            }
            myDB = args.db;
            collName = args.collName;
            first = false;

            if (workload !== currentWorkload) {
                args.states[args.startState].call(args.data, myDB, collName);
            }
        });

        // Runs an interleaving of the specified workloads
        for (var i = 0; i < iterations; ++i) {
            var args = configs[currentWorkload];
            args.states[currentState].call(args.data, myDB, collName);

            // Transition to another valid state of the current workload,
            // with probability '1 - mixProb'
            if (Random.rand() >= mixProb) {
                var nextState =
                    fsm._getWeightedRandomChoice(args.transitions[currentState], Random.rand());
                currentState = nextState;
                continue;
            }

            // Transition to a state of another workload with probability 'mixProb'
            var otherStates = [];
            workloads.forEach(function(workload) {
                if (workload === currentWorkload) {
                    return;
                }

                var args = configs[workload];
                Object.keys(args.states).forEach(function(state) {
                    if (state !== args.startState) {
                        otherStates.push({workload: workload, state: state});
                    }
                });
            });

            var next = getRandomElem(otherStates, Random.rand());
            currentWorkload = next.workload;
            currentState = next.state;
        }
    }

    function getRandomElem(items, randVal) {
        assert.gt(items.length, 0);
        return items[Math.floor(randVal * items.length)];
    }

    return {run: runCombinedFSM};

})();
