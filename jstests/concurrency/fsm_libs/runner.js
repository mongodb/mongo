load('jstests/libs/parallelTester.js');
load('jstests/concurrency/fsm_libs/assert.js');
load('jstests/concurrency/fsm_libs/utils.js');
load('jstests/concurrency/fsm_libs/worker_thread.js');


/** extendWorkload usage:
 *
 * $config = extendWorkload($config, function($config, $super) {
 *   // ... modify $config ...
 *   $config.foo = function() { // override a method
 *     $super.foo.call(this, arguments); // call super
 *   };
 *   return $config;
 * });
 */
function extendWorkload($config, callback) {
    assert.eq(2, arguments.length,
              "extendWorkload must be called with 2 arguments: $config and callback");
    assert.eq('function', typeof callback,
              "2nd argument to extendWorkload must be a callback");
    assert.eq(2, callback.length,
              "2nd argument to extendWorkload must take 2 arguments: $config and $super");
    var parsedSuperConfig = parseConfig($config);
    var childConfig = Object.extend({}, parsedSuperConfig, true);
    return callback(childConfig, parsedSuperConfig);
}

function runWorkloadsSerially(workloads, clusterOptions) {
    if (typeof workloads === 'string') {
        workloads = [workloads];
    }
    assert.gt(workloads.length, 0);
    workloads.forEach(function(workload) {
        // 'workload' is a JS file expected to set the global $config variable to an object.
        load(workload);
        assert.neq(typeof $config, 'undefined');

        _runWorkload(workload, $config, clusterOptions);
    });
}

function runWorkloadsInParallel(workloads, clusterOptions) {
    assert.gt(workloads.length, 0);

    var context = {};
    workloads.forEach(function(workload) {
        // 'workload' is a JS file expected to set the global $config variable to an object.
        load(workload);
        assert.neq(typeof $config, 'undefined');
        context[workload] = { config: $config };
    });

    _runAllWorkloads(workloads, context, clusterOptions);
}

function runMixtureOfWorkloads(workloads, clusterOptions) {
    assert.gt(workloads.length, 0);

    var context = {};
    workloads.forEach(function(workload) {
        // 'workload' is a JS file expected to set the global $config variable to an object.
        load(workload);
        assert.neq(typeof $config, 'undefined');
        context[workload] = { config: $config };
    });

    clusterOptions = Object.extend({}, clusterOptions, true); // defensive deep copy
    clusterOptions.sameDB = true;
    clusterOptions.sameCollection = true;

    var cluster = setupCluster(clusterOptions, 'fakedb');
    globalAssertLevel = AssertLevel.ALWAYS;

    var cleanup = [];
    var errors = [];

    try {
        prepareCollections(workloads, context, cluster, clusterOptions);
        cleanup = setUpWorkloads(workloads, context);

        var threads = makeAllThreads(workloads, context, clusterOptions, true);

        joinThreads(threads).forEach(function(err) {
            errors.push(err);
        });

    } finally {
        // TODO: does order of calling 'config.teardown' matter?
        cleanup.forEach(function(teardown) {
            try {
                teardown.fn.call(teardown.data, teardown.db, teardown.collName);
            } catch (err) {
                print('Teardown function threw an exception:\n' + err.stack);
            }
        });

        cluster.teardown();
    }

    throwError(errors);
}

// Validate the config object and return a normalized copy of it.
// Normalized means all optional parameters are set to their default values,
// and any parameters that need to be coerced have been coerced.
function parseConfig(config) {
    // make a deep copy so we can mutate config without surprising the caller
    config = Object.extend({}, config, true);
    var allowedKeys = [
        'data',
        'iterations',
        'setup',
        'startState',
        'states',
        'teardown',
        'threadCount',
        'transitions'
    ];
    Object.keys(config).forEach(function(k) {
        assert.gte(allowedKeys.indexOf(k), 0,
                   "invalid config parameter: " + k + ". valid parameters are: " +
                   tojson(allowedKeys));
    });

    assert.eq('number', typeof config.threadCount);

    assert.eq('number', typeof config.iterations);

    config.startState = config.startState || 'init';
    assert.eq('string', typeof config.startState);

    assert.eq('object', typeof config.states);
    assert.gt(Object.keys(config.states).length, 0);
    Object.keys(config.states).forEach(function(k) {
        assert.eq('function', typeof config.states[k],
                   "config.states." + k + " is not a function");
        assert.eq(2, config.states[k].length,
                  "state functions should accept 2 parameters: db and collName");
    });

    // assert all states mentioned in config.transitions are present in config.states
    assert.eq('object', typeof config.transitions);
    assert.gt(Object.keys(config.transitions).length, 0);
    Object.keys(config.transitions).forEach(function(fromState) {
        assert(config.states.hasOwnProperty(fromState),
               "config.transitions contains a state not in config.states: " + fromState);

        assert.gt(Object.keys(config.transitions[fromState]).length, 0);
        Object.keys(config.transitions[fromState]).forEach(function(toState) {
            assert(config.states.hasOwnProperty(toState),
                   "config.transitions." + fromState +
                   " contains a state not in config.states: " + toState);
            assert.eq('number', typeof config.transitions[fromState][toState],
                      "transitions." + fromState + "." + toState + " should be a number");
        });
    });

    config.setup = config.setup || function(){};
    assert.eq('function', typeof config.setup);

    config.teardown = config.teardown || function(){};
    assert.eq('function', typeof config.teardown);

    config.data = config.data || {};
    assert.eq('object', typeof config.data);

    return config;
}

function setupCluster(clusterOptions, dbName) {
    var cluster = {};

    var allowedKeys = [
        'masterSlave',
        'replication',
        'sameCollection',
        'sameDB',
        'seed',
        'sharded'
    ];
    Object.keys(clusterOptions).forEach(function(opt) {
        assert(0 <= allowedKeys.indexOf(opt),
               "invalid option: " + tojson(opt) + ". valid options are: " + tojson(allowedKeys));
    });

    var verbosityLevel = 1;
    if (clusterOptions.sharded) {
        // TODO: allow 'clusterOptions' to specify the number of shards
        var shardConfig = {
            shards: 2,
            mongos: 1,
            verbose: verbosityLevel
        };

        // TODO: allow 'clusterOptions' to specify an 'rs' config
        if (clusterOptions.replication) {
            shardConfig.rs = {
                nodes: 3,
                verbose: verbosityLevel
            };
        }

        var st = new ShardingTest(shardConfig);
        st.stopBalancer();
        var mongos = st.s;

        clusterOptions.addr = mongos.host;
        cluster.db = mongos.getDB(dbName);
        cluster.shardCollection = function() {
            st.shardColl.apply(st, arguments);
        };
        cluster.teardown = function() {
            st.stop();
        };
    } else if (clusterOptions.replication) {
        // TODO: allow 'clusterOptions' to specify the number of nodes
        var replSetConfig = {
            nodes: 3,
            nodeOptions: { verbose: verbosityLevel }
        };

        var rst = new ReplSetTest(replSetConfig);
        rst.startSet();

        // Send the replSetInitiate command and wait for initiation
        rst.initiate();
        rst.awaitSecondaryNodes();

        var primary = rst.getPrimary();

        clusterOptions.addr = primary.host;
        cluster.db = primary.getDB(dbName);
        cluster.teardown = function() {
            rst.stopSet();
        };
    } else if (clusterOptions.masterSlave) {
        var rt = new ReplTest('replTest');

        var master = rt.start(true);
        var slave = rt.start(false);

        master.adminCommand({ setParameter: 1, logLevel: verbosityLevel });
        slave.adminCommand({ setParameter: 1, logLevel: verbosityLevel });

        clusterOptions.addr = master.host;
        cluster.db = master.getDB(dbName);
        cluster.teardown = function() {
            rt.stop();
        };
    } else { // standalone server
        cluster.db = db.getSiblingDB(dbName);
        cluster.db.adminCommand({ setParameter: 1, logLevel: verbosityLevel });
        cluster.teardown = function() {};
    }

    return cluster;
}

function _runWorkload(workload, config, clusterOptions) {
    var context = {};
    context[workload] = { config: config };
    _runAllWorkloads([workload], context, clusterOptions);
}

// TODO: give this function a more descriptive name?
// Calls the 'config.setup' function for each workload, and returns
// an array of 'config.teardown' functions to execute with the appropriate
// arguments. Note that the implementation relies on having 'db' and 'collName'
// set as properties on context[workload].
function setUpWorkloads(workloads, context) {
    return workloads.map(function(workload) {
        var myDB = context[workload].db;
        var collName = context[workload].collName;

        var config = context[workload].config;
        config = parseConfig(config);
        config.setup.call(config.data, myDB, collName);

        return {
            fn: config.teardown,
            data: config.data,
            db: myDB,
            collName: collName
        };
    });
}

function prepareCollections(workloads, context, cluster, clusterOptions) {
    var dbName, collName, myDB;
    var firstWorkload = true;

    // Clean up the state left behind by other tests in the concurrency suite
    // to avoid having too many open files
    db.dropDatabase();

    workloads.forEach(function(workload) {
        if (firstWorkload || !clusterOptions.sameCollection) {
            if (firstWorkload || !clusterOptions.sameDB) {
                dbName = uniqueDBName();
            }
            collName = uniqueCollName();

            myDB = cluster.db.getSiblingDB(dbName);
            myDB[collName].drop();

            if (clusterOptions.sharded) {
                // TODO: allow 'clusterOptions' to specify the shard key and split
                cluster.shardCollection(myDB[collName], { _id: 'hashed' }, false);
            }
        }

        context[workload].db = myDB;
        context[workload].dbName = dbName;
        context[workload].collName = collName;

        firstWorkload = false;
    });
}

/* This is the function that most other run*Workload* functions delegate to.
 * It takes an array of workload filenames and runs them all in parallel.
 *
 * TODO: document the other two parameters
 */
function _runAllWorkloads(workloads, context, clusterOptions) {
    clusterOptions = Object.extend({}, clusterOptions, true); // defensive deep copy
    var cluster = setupCluster(clusterOptions, 'fakedb');

    // Determine how strong to make assertions while simultaneously executing different workloads
    var assertLevel = AssertLevel.OWN_DB;
    if (clusterOptions.sameDB) {
        // The database is shared by multiple workloads, so only make the asserts
        // that apply when the collection is owned by an individual workload
        assertLevel = AssertLevel.OWN_COLL;
    }
    if (clusterOptions.sameCollection) {
        // The collection is shared by multiple workloads, so only make the asserts
        // that always apply
        assertLevel = AssertLevel.ALWAYS;
    }
    globalAssertLevel = assertLevel;

    var cleanup = [];
    var errors = [];

    try {
        prepareCollections(workloads, context, cluster, clusterOptions);
        cleanup = setUpWorkloads(workloads, context);

        var threads = makeAllThreads(workloads, context, clusterOptions, false);

        joinThreads(threads).forEach(function(err) {
            errors.push(err);
        });
    } finally {
        // TODO: does order of calling 'config.teardown' matter?
        cleanup.forEach(function(teardown) {
            try {
                teardown.fn.call(teardown.data, teardown.db, teardown.collName);
            } catch (err) {
                print('Teardown function threw an exception:\n' + err.stack);
            }
        });

        cluster.teardown();
    }

    throwError(errors);
}

function makeAllThreads(workloads, context, clusterOptions, compose) {
    var threadFn, getWorkloads;
    if (compose) {
        // Worker threads need to load() all workloads when composed
        threadFn = workerThread.composed;
        getWorkloads = function() { return workloads; };
    } else {
        // Worker threads only need to load() the specified workload
        threadFn = workerThread.fsm;
        getWorkloads = function(workload) { return [workload]; };
    }

    function sumRequestedThreads() {
        return Array.sum(workloads.map(function(wl) {
            return context[wl].config.threadCount;
        }));
    }

    // TODO: pick a better cap for maximum allowed threads?
    var maxAllowedThreads = 100;
    var requestedNumThreads = sumRequestedThreads();
    if (requestedNumThreads > maxAllowedThreads) {
        print('\n\ntoo many threads requested: ' + requestedNumThreads);
        // Scale down the requested '$config.threadCount' values to make
        // them sum to less than 'maxAllowedThreads'
        var factor = maxAllowedThreads / requestedNumThreads;
        workloads.forEach(function(workload) {
            var threadCount = context[workload].config.threadCount;
            threadCount = Math.floor(factor * threadCount);
            threadCount = Math.max(1, threadCount); // ensure workload is executed
            context[workload].config.threadCount = threadCount;
        });
    }
    var numThreads = sumRequestedThreads();
    print('using num threads: ' + numThreads);
    assert.lte(numThreads, maxAllowedThreads);

    var latch = new CountDownLatch(numThreads);

    var threads = [];

    jsTest.log(workloads.join('\n'));
    Random.setRandomSeed(clusterOptions.seed);

    var tid = 0;
    workloads.forEach(function(workload) {
        var workloadsToLoad = getWorkloads(workload);
        var config = context[workload].config;

        for (var i = 0; i < config.threadCount; ++i) {
            var args = {
                tid: tid++,
                latch: latch,
                dbName: context[workload].dbName,
                collName: context[workload].collName,
                clusterOptions: clusterOptions,
                seed: Random.randInt(1e13), // contains range of Date.getTime()
                globalAssertLevel: globalAssertLevel
            };

            // Wrap threadFn with try/finally to make sure it always closes the db connection
            // that is implicitly created within the thread's scope.
            var guardedThreadFn = function(threadFn, args) {
                try {
                    return threadFn.apply(this, args);
                } finally {
                    db = null;
                    gc();
                }
            };

            var t = new ScopedThread(guardedThreadFn, threadFn, [workloadsToLoad, args]);
            threads.push(t);
            t.start();

            // Wait a little before starting the next thread
            // to avoid creating new connections at the same time
            sleep(10);
        }
    });

    var failedThreadIndexes = [];
    while (latch.getCount() > 0) {
        threads.forEach(function(t, i) {
            if (t.hasFailed() && !Array.contains(failedThreadIndexes, i)) {
                failedThreadIndexes.push(i);
                latch.countDown();
            }
        });

        sleep(100);
    }

    var failedThreads = failedThreadIndexes.length;
    if (failedThreads > 0) {
        print(failedThreads + ' thread(s) threw a JS or C++ exception while spawning');
    }

    var allowedFailure = 0.2;
    if (failedThreads / numThreads > allowedFailure) {
        throw new Error('Too many worker threads failed to spawn - aborting');
    }

    return threads;
}

function joinThreads(workerThreads) {
    var workerErrs = [];

    workerThreads.forEach(function(t) {
        t.join();

        var data = t.returnData();
        if (data && !data.ok) {
            workerErrs.push(data);
        }
    });

    return workerErrs;
}

function throwError(workerErrs) {

    // Returns an array containing all unique values from the specified array
    // and their corresponding number of occurrences in the original array.
    function freqCount(arr) {
        var unique = [];
        var freqs = [];

        arr.forEach(function(item) {
            var i = unique.indexOf(item);
            if (i < 0) {
                unique.push(item);
                freqs.push(1);
            } else {
                freqs[i]++;
            }
        });

        return unique.map(function(value, i) {
            return { value: value, freq: freqs[i] };
        });
    }

    // Indents a multiline string with the specified number of spaces.
    function indent(str, size) {
        var prefix = new Array(size + 1).join(' ');
        return prefix + str.split('\n').join('\n' + prefix);
    }

    function pluralize(str, num) {
        var suffix = num > 1 ? 's' : '';
        return num + ' ' + str + suffix;
    }

    function prepareMsg(stackTraces) {
        var uniqueTraces = freqCount(stackTraces);
        var numUniqueTraces = uniqueTraces.length;

        // Special case message when threads all have the same trace
        if (numUniqueTraces === 1) {
            return pluralize('thread', stackTraces.length) + ' threw\n\n' +
                   indent(uniqueTraces[0].value, 8);
        }

        var summary = pluralize('thread', stackTraces.length) + ' threw ' +
                      numUniqueTraces + ' different exceptions:\n\n';

        return summary + uniqueTraces.map(function(obj) {
            var line = pluralize('thread', obj.freq) + ' threw\n';
            return indent(line + obj.value, 8);
        }).join('\n\n');
    }

    if (workerErrs.length > 0) {
        var stackTraces = workerErrs.map(function(e) {
            return e.stack || e.err;
        });

        var err = new Error(prepareMsg(stackTraces) + '\n');

        // Avoid having any stack traces omitted from the logs
        var maxLogLine = 10 * 1024; // 10KB

        // Check if the combined length of the error message and the stack traces
        // exceeds the maximum line-length the shell will log
        if (err.stack.length >= maxLogLine) {
            print(err.stack);
            throw new Error('stack traces would have been snipped, see logs');
        }

        throw err;
    }
}

workerThread.fsm = function(workloads, args) {
    load('jstests/concurrency/fsm_libs/worker_thread.js'); // for workerThread.main
    load('jstests/concurrency/fsm_libs/fsm.js'); // for fsm.run

    return workerThread.main(workloads, args, function(configs) {
        var workloads = Object.keys(configs);
        assert.eq(1, workloads.length);
        fsm.run(configs[workloads[0]]);
    });
};

workerThread.composed = function(workloads, args) {
    load('jstests/concurrency/fsm_libs/worker_thread.js'); // for workerThread.main
    load('jstests/concurrency/fsm_libs/composer.js'); // for composer.run

    return workerThread.main(workloads, args, function(configs) {
        // TODO: make mixing probability configurable
        composer.run(workloads, configs, 0.1);
    });
};
