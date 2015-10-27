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
        var allowedKeys = ['dbNamePrefix'];
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

        if (typeof options.dbNamePrefix !== 'undefined') {
            assert.eq('string', typeof options.dbNamePrefix,
                      'expected dbNamePrefix to be a string');
        }

        return options;
    }

    function validateCleanupOptions(options) {
        var allowedKeys = [
            'dropDatabaseBlacklist',
            'keepExistingDatabases'
        ];

        Object.keys(options).forEach(function(option) {
            assert.contains(option, allowedKeys,
                            'invalid option: ' + tojson(option) +
                            '; valid options are: ' + tojson(allowedKeys));
        });

        if (typeof options.dropDatabaseBlacklist !== 'undefined') {
            assert(Array.isArray(options.dropDatabaseBlacklist),
                   'expected dropDatabaseBlacklist to be an array');
        }

        if (typeof options.keepExistingDatabases !== 'undefined') {
            assert.eq('boolean', typeof options.keepExistingDatabases,
                      'expected keepExistingDatabases to be a boolean');
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

    function prepareCollections(workloads, context, cluster, clusterOptions, executionOptions) {
        var dbName, collName, myDB;
        var firstWorkload = true;

        workloads.forEach(function(workload) {
            // Workloads cannot have a shardKey if sameCollection is specified
            if (clusterOptions.sameCollection &&
                    cluster.isSharded() &&
                    context[workload].config.data.shardKey) {
                throw new Error('cannot specify a shardKey with sameCollection option');
            }
            if (firstWorkload || !clusterOptions.sameCollection) {
                if (firstWorkload || !clusterOptions.sameDB) {
                    dbName = uniqueDBName(executionOptions.dbNamePrefix);
                }
                collName = uniqueCollName();

                myDB = cluster.getDB(dbName);
                myDB[collName].drop();

                if (cluster.isSharded()) {
                    var shardKey = context[workload].config.data.shardKey || { _id: 'hashed' };
                    // TODO: allow workload config data to specify split
                    cluster.shardCollection(myDB[collName], shardKey, false);
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
                // Prepend the error message to the stack trace because it
                // isn't automatically included in SpiderMonkey stack traces.
                return e.err + '\n\n' + e.stack;
            });

            var err = new Error(prepareMsg(stackTraces) + '\n');

            // Avoid having any stack traces omitted from the logs
            var maxLogLine = 10 * 1024; // 10KB

            // Check if the combined length of the error message and the stack traces
            // exceeds the maximum line-length the shell will log.
            if ((err.message.length + err.stack.length) >= maxLogLine) {
                print(err.message);
                print(err.stack);
                throw new Error('stack traces would have been snipped, see logs');
            }

            throw err;
        }
    }

    function setupWorkload(workload, context, cluster) {
        var myDB = context[workload].db;
        var collName = context[workload].collName;

        var config = context[workload].config;
        config.setup.call(config.data, myDB, collName, cluster);
    }

    function teardownWorkload(workload, context, cluster) {
        var myDB = context[workload].db;
        var collName = context[workload].collName;

        var config = context[workload].config;
        config.teardown.call(config.data, myDB, collName, cluster);
    }

    function runWorkloads(workloads,
                          clusterOptions,
                          executionMode,
                          executionOptions,
                          cleanupOptions) {
        assert.gt(workloads.length, 0, 'need at least one workload to run');

        executionMode = validateExecutionMode(executionMode);
        Object.freeze(executionMode); // immutable after validation (and normalization)

        Object.freeze(executionOptions); // immutable prior to validation
        validateExecutionOptions(executionMode, executionOptions);

        Object.freeze(cleanupOptions); // immutable prior to validation
        validateCleanupOptions(cleanupOptions);

        if (executionMode.composed) {
            clusterOptions.sameDB = true;
            clusterOptions.sameCollection = true;
        }

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

        // List of DBs that will not be dropped
        var dbBlacklist = ['admin', 'config', 'local', '$external'];
        if (cleanupOptions.dropDatabaseBlacklist) {
            dbBlacklist = dbBlacklist.concat(cleanupOptions.dropDatabaseBlacklist);
        }
        if (!cleanupOptions.keepExistingDatabases) {
            dropAllDatabases(cluster.getDB('test'), dbBlacklist);
        }

        var maxAllowedConnections = 100;
        Random.setRandomSeed(clusterOptions.seed);

        try {
            var schedule = scheduleWorkloads(workloads, executionMode, executionOptions);

            // Print out the entire schedule of workloads to make it easier to run the same
            // schedule when debugging test failures.
            jsTest.log('The entire schedule of FSM workloads:');

            // Note: We use printjsononeline (instead of just plain printjson) to make it
            // easier to reuse the output in variable assignments.
            printjsononeline(schedule);

            jsTest.log('End of schedule');

            schedule.forEach(function(workloads) {
                var cleanup = [];
                var errors = [];
                var teardownFailed = false;
                var startTime = new Date(); // Initialize in case setupWorkload fails below
                var endTime, totalTime;

                jsTest.log(workloads.join('\n'));

                prepareCollections(workloads, context, cluster, clusterOptions, executionOptions);

                try {
                    workloads.forEach(function(workload) {
                        setupWorkload(workload, context, cluster);
                        cleanup.push(workload);
                    });

                    startTime = new Date();
                    threadMgr.init(workloads, context, maxAllowedConnections);

                    try {
                        threadMgr.spawnAll(cluster, executionOptions);
                        threadMgr.checkFailed(0.2);
                    } finally {
                        // Threads must be joined before destruction, so do this
                        // even in the presence of exceptions.
                        errors = threadMgr.joinAll();
                    }
                } finally {
                    endTime = new Date();
                    cleanup.forEach(function(workload) {
                        try {
                            teardownWorkload(workload, context, cluster);
                        } catch (err) {
                            // Prepend the error message to the stack trace because it
                            // isn't automatically included in SpiderMonkey stack traces.
                            var message = err.message + '\n\n' + err.stack;

                            print('Workload teardown function threw an exception:\n' + message);
                            teardownFailed = true;
                        }
                    });

                    totalTime = endTime.getTime() - startTime.getTime();
                    if (!executionMode.parallel && !executionMode.composed) {
                        jsTest.log(workloads[0] + ': Workload completed in ' + totalTime + ' ms');
                    }
                }

                // Only drop the collections/databases if all the workloads ran successfully
                if (!errors.length && !teardownFailed) {
                    // For sharded clusters, enable a fail point that allows
                    // dropCollection to wait longer to acquire the distributed lock.
                    // This prevents tests from failing if the distributed lock is
                    // already held by the balancer or by a workload operation. The
                    // increased wait is shorter than the distributed-lock-takeover
                    // period because otherwise the node would be assumed to be down
                    // and the lock would be overtaken.
                    if (cluster.isSharded()) {
                        cluster.increaseDropDistLockTimeout();
                    }
                    cleanupWorkloadData(workloads, context, clusterOptions);
                    if (cluster.isSharded()) {
                        cluster.resetDropDistLockTimeout();
                    }
                }

                throwError(errors);

                if (teardownFailed) {
                    throw new Error('workload teardown function(s) failed, see logs');
                }

                // Ensure that secondaries have caught up for workload teardown (SERVER-18878)
                cluster.awaitReplication();
            });
        } finally {
            cluster.teardown();
        }
    }

    return {
        serial: function serial(workloads, clusterOptions, executionOptions, cleanupOptions) {
            clusterOptions = clusterOptions || {};
            executionOptions = executionOptions || {};
            cleanupOptions = cleanupOptions || {};

            runWorkloads(workloads, clusterOptions, {}, executionOptions, cleanupOptions);
        },

        parallel: function parallel(workloads, clusterOptions, executionOptions, cleanupOptions) {
            clusterOptions = clusterOptions || {};
            executionOptions = executionOptions || {};
            cleanupOptions = cleanupOptions || {};

            runWorkloads(workloads,
                         clusterOptions,
                         { parallel: true },
                         executionOptions,
                         cleanupOptions);
        },

        composed: function composed(workloads, clusterOptions, executionOptions, cleanupOptions) {
            clusterOptions = clusterOptions || {};
            executionOptions = executionOptions || {};
            cleanupOptions = cleanupOptions || {};

            runWorkloads(workloads,
                         clusterOptions,
                         { composed: true },
                         executionOptions,
                         cleanupOptions);
        }
    };

})();

var runWorkloadsSerially = runner.serial;
var runWorkloadsInParallel = runner.parallel;
var runCompositionOfWorkloads = runner.composed;
