'use strict';

load('jstests/concurrency/fsm_libs/assert.js');
load('jstests/concurrency/fsm_libs/cluster.js');
load('jstests/concurrency/fsm_libs/name_utils.js');
load('jstests/concurrency/fsm_libs/parse_config.js');
load('jstests/concurrency/fsm_libs/thread_mgr.js');

var runner = (function() {

    function validateExecutionMode(mode) {
        var allowedKeys = [
            'composed',
            'parallel'
        ];

        Object.keys(mode).forEach(function(option) {
            assert.contains(option, allowedKeys,
                            'invalid option: ' + tojson(option) +
                            '; valid options are: ' + tojson(allowedKeys));
        });

        mode.composed = mode.composed || false;
        assert.eq('boolean', typeof mode.composed);

        mode.parallel = mode.parallel || false;
        assert.eq('boolean', typeof mode.parallel);

        assert(!mode.composed || !mode.parallel,
               "properties 'composed' and 'parallel' cannot both be true");

        return mode;
    }

    function validateExecutionOptions(mode, options) {
        var allowedKeys = [];
        if (mode.parallel || mode.composed) {
            allowedKeys.push('numSubsets');
            allowedKeys.push('subsetSize');
        }
        if (mode.composed) {
            allowedKeys.push('composeProb');
            allowedKeys.push('iterations');
        }

        Object.keys(options).forEach(function(option) {
            assert.contains(option, allowedKeys,
                            'invalid option: ' + tojson(option) +
                            '; valid options are: ' + tojson(allowedKeys));
        });

        if (typeof options.subsetSize !== 'undefined') {
            assert.eq('number', typeof options.subsetSize);
            assert.gt(options.subsetSize, 1);
            assert.eq(options.subsetSize, Math.floor(options.subsetSize),
                      'expected subset size to be an integer');
        }

        if (typeof options.numSubsets !== 'undefined') {
            assert.eq('number', typeof options.numSubsets);
            assert.gt(options.numSubsets, 0);
            assert.eq(options.numSubsets, Math.floor(options.numSubsets),
                      'expected number of subsets to be an integer');
        }

        if (typeof options.iterations !== 'undefined') {
            assert.eq('number', typeof options.iterations);
            assert.gt(options.iterations, 0);
            assert.eq(options.iterations, Math.floor(options.iterations),
                      'expected number of iterations to be an integer');
        }

        if (typeof options.composeProb !== 'undefined') {
            assert.eq('number', typeof options.composeProb);
            assert.gt(options.composeProb, 0);
            assert.lte(options.composeProb, 1);
        }

        return options;
    }

    /**
     * Returns an array containing sets of workloads.
     * Each set of workloads is executed together according to the execution mode.
     *
     * For example, returning [ [ workload1, workload2 ], [ workload2, workload3 ] ]
     * when 'executionMode.parallel == true' causes workloads #1 and #2 to be
     * executed simultaneously, followed by workloads #2 and #3 together.
     */
    function scheduleWorkloads(workloads, executionMode, executionOptions) {
        if (!executionMode.composed && !executionMode.parallel) { // serial execution
            return Array.shuffle(workloads).map(function(workload) {
                return [workload]; // run each workload by itself
            });
        }

        var schedule = [];

        // Take 'numSubsets' random subsets of the workloads, each
        // of size 'subsetSize'. Each workload must get scheduled
        // once before any workload can be scheduled again.
        var subsetSize = executionOptions.subsetSize || 10;

        // If the number of subsets is not specified, then have each
        // workload get scheduled 2 to 3 times.
        var numSubsets = executionOptions.numSubsets;
        if (!numSubsets) {
            numSubsets = Math.ceil(2.5 * workloads.length / subsetSize);
        }

        workloads = workloads.slice(0); // copy
        workloads = Array.shuffle(workloads);

        var start = 0;
        var end = subsetSize;

        while (schedule.length < numSubsets) {
            schedule.push(workloads.slice(start, end));

            start = end;
            end += subsetSize;

            // Check if there are not enough elements remaining in
            // 'workloads' to make a subset of size 'subsetSize'.
            if (end > workloads.length) {
                // Re-shuffle the beginning of the array, and prepend it
                // with the workloads that have not been scheduled yet.
                var temp = Array.shuffle(workloads.slice(0, start));
                for (var i = workloads.length - 1; i >= start; --i) {
                    temp.unshift(workloads[i]);
                }
                workloads = temp;

                start = 0;
                end = subsetSize;
            }
        }

        return schedule;
    }

    function prepareCollections(workloads, context, cluster, clusterOptions) {
        var dbName, collName, myDB;
        var firstWorkload = true;

        workloads.forEach(function(workload) {
            if (firstWorkload || !clusterOptions.sameCollection) {
                if (firstWorkload || !clusterOptions.sameDB) {
                    dbName = uniqueDBName();
                }
                collName = uniqueCollName();

                myDB = cluster.getDB(dbName);
                myDB[collName].drop();

                if (cluster.isSharded()) {
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

    function dropAllDatabases(db, blacklist) {
        var res = db.adminCommand('listDatabases');
        assert.commandWorked(res);

        res.databases.forEach(function(dbInfo) {
            if (!Array.contains(blacklist, dbInfo.name)) {
                var res = db.getSiblingDB(dbInfo.name).dropDatabase();
                assert.commandWorked(res);
                assert.eq(dbInfo.name, res.dropped);
            }
        });
    }

    function cleanupWorkloadData(workloads, context, clusterOptions) {
        // If no other workloads will be using this collection,
        // then drop it to avoid having too many files open
        if (!clusterOptions.sameCollection) {
            workloads.forEach(function(workload) {
                var config = context[workload];
                config.db[config.collName].drop();
            });
        }

        // If no other workloads will be using this database,
        // then drop it to avoid having too many files open
        if (!clusterOptions.sameDB) {
            workloads.forEach(function(workload) {
                var config = context[workload];
                config.db.dropDatabase();
            });
        }
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

    function setupWorkload(workload, context) {
        var myDB = context[workload].db;
        var collName = context[workload].collName;

        var config = context[workload].config;
        config.setup.call(config.data, myDB, collName);
    }

    function teardownWorkload(workload, context) {
        var myDB = context[workload].db;
        var collName = context[workload].collName;

        var config = context[workload].config;
        config.teardown.call(config.data, myDB, collName);
    }

    function runWorkloads(workloads, clusterOptions, executionMode, executionOptions) {
        assert.gt(workloads.length, 0, 'need at least one workload to run');

        executionMode = validateExecutionMode(executionMode);
        Object.freeze(executionMode); // immutable after validation (and normalization)

        Object.freeze(executionOptions); // immutable prior to validation
        validateExecutionOptions(executionMode, executionOptions);

        if (executionMode.composed) {
            clusterOptions.sameDB = true;
            clusterOptions.sameCollection = true;
        }
        Object.freeze(clusterOptions);

        // Determine how strong to make assertions while simultaneously executing
        // different workloads
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

        var context = {};
        workloads.forEach(function(workload) {
            load(workload); // for $config
            assert.neq('undefined', typeof $config, '$config was not defined by ' + workload);
            context[workload] = { config: parseConfig($config) };
        });

        var threadMgr = new ThreadManager(clusterOptions, executionMode);

        var cluster = new Cluster(clusterOptions);
        cluster.setup();

        // Clean up the state left behind by other tests in the concurrency suite
        // to avoid having too many open files
        dropAllDatabases(cluster.getDB('test'),
                         ['admin', 'config', 'local', '$external'] /* blacklist */);

        var maxAllowedConnections = 100;
        Random.setRandomSeed(clusterOptions.seed);

        try {
            var schedule = scheduleWorkloads(workloads, executionMode, executionOptions);
            schedule.forEach(function(workloads) {
                var cleanup = [];
                var errors = [];
                var teardownFailed = false;
                var startTime, endTime, totalTime;

                jsTest.log(workloads.join('\n'));

                prepareCollections(workloads, context, cluster, clusterOptions);

                try {
                    workloads.forEach(function(workload) {
                        setupWorkload(workload, context);
                        cleanup.push(workload);
                    });

                    startTime = new Date();
                    threadMgr.init(workloads, context, maxAllowedConnections);
                    threadMgr.spawnAll(cluster.getHost(), executionOptions);
                    threadMgr.checkFailed(0.2);

                    errors = threadMgr.joinAll();
                } finally {
                    endTime = new Date();
                    cleanup.forEach(function(workload) {
                        try {
                            teardownWorkload(workload, context);
                        } catch (err) {
                            print('Workload teardown function threw an exception:\n' + err.stack);
                            teardownFailed = true;
                        }
                    });

                    totalTime = endTime.getTime() - startTime.getTime();
                    assert.gte(totalTime, 0);
                    if (!executionMode.parallel && !executionMode.composed) {
                        jsTest.log(workloads[0] + ': Workload completed in ' + totalTime + ' ms');
                    }
                }

                // Only drop the collections/databases if all the workloads ran successfully
                if (!errors.length && !teardownFailed) {
                    cleanupWorkloadData(workloads, context, clusterOptions);
                }

                throwError(errors);

                if (teardownFailed) {
                    throw new Error('workload teardown function(s) failed, see logs');
                }
            });
        } finally {
            cluster.teardown();
        }
    }

    return {
        serial: function serial(workloads, clusterOptions, executionOptions) {
            clusterOptions = clusterOptions || {};
            executionOptions = executionOptions || {};

            runWorkloads(workloads, clusterOptions, {}, executionOptions);
        },

        parallel: function parallel(workloads, clusterOptions, executionOptions) {
            clusterOptions = clusterOptions || {};
            executionOptions = executionOptions || {};

            runWorkloads(workloads, clusterOptions, { parallel: true }, executionOptions);
        },

        composed: function composed(workloads, clusterOptions, executionOptions) {
            clusterOptions = clusterOptions || {};
            executionOptions = executionOptions || {};

            runWorkloads(workloads, clusterOptions, { composed: true }, executionOptions);
        }
    };

})();

var runWorkloadsSerially = runner.serial;
var runWorkloadsInParallel = runner.parallel;
var runCompositionOfWorkloads = runner.composed;
