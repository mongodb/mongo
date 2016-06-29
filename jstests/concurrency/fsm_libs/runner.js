'use strict';

load('jstests/concurrency/fsm_libs/assert.js');
load('jstests/concurrency/fsm_libs/cluster.js');
load('jstests/concurrency/fsm_libs/errors.js');  // for IterationEnd
load('jstests/concurrency/fsm_libs/parse_config.js');
load('jstests/concurrency/fsm_libs/thread_mgr.js');
load('jstests/concurrency/fsm_utils/name_utils.js');  // for uniqueCollName and uniqueDBName
load('jstests/concurrency/fsm_utils/setup_teardown_functions.js');

var runner = (function() {

    function validateExecutionMode(mode) {
        var allowedKeys = ['composed', 'parallel', 'serial'];

        Object.keys(mode).forEach(function(option) {
            assert.contains(option,
                            allowedKeys,
                            'invalid option: ' + tojson(option) + '; valid options are: ' +
                                tojson(allowedKeys));
        });

        mode.composed = mode.composed || false;
        assert.eq('boolean', typeof mode.composed);

        mode.parallel = mode.parallel || false;
        assert.eq('boolean', typeof mode.parallel);

        mode.serial = mode.serial || false;
        assert.eq('boolean', typeof mode.serial);

        var numEnabledModes = 0;
        Object.keys(mode).forEach(key => {
            if (mode[key]) {
                numEnabledModes++;
            }
        });
        assert.eq(
            1, numEnabledModes, "One and only one execution mode can be enabled " + tojson(mode));

        return mode;
    }

    function validateExecutionOptions(mode, options) {
        var allowedKeys =
            ['backgroundWorkloads', 'dbNamePrefix', 'iterationMultiplier', 'threadMultiplier'];

        if (mode.parallel || mode.composed) {
            allowedKeys.push('numSubsets');
            allowedKeys.push('subsetSize');
        }
        if (mode.composed) {
            allowedKeys.push('composeProb');
            allowedKeys.push('iterations');
        }

        Object.keys(options).forEach(function(option) {
            assert.contains(option,
                            allowedKeys,
                            'invalid option: ' + tojson(option) + '; valid options are: ' +
                                tojson(allowedKeys));
        });

        if (typeof options.subsetSize !== 'undefined') {
            assert(Number.isInteger(options.subsetSize), 'expected subset size to be an integer');
            assert.gt(options.subsetSize, 1);
        }

        if (typeof options.numSubsets !== 'undefined') {
            assert(Number.isInteger(options.numSubsets),
                   'expected number of subsets to be an integer');
            assert.gt(options.numSubsets, 0);
        }

        if (typeof options.iterations !== 'undefined') {
            assert(Number.isInteger(options.iterations),
                   'expected number of iterations to be an integer');
            assert.gt(options.iterations, 0);
        }

        if (typeof options.composeProb !== 'undefined') {
            assert.eq('number', typeof options.composeProb);
            assert.gt(options.composeProb, 0);
            assert.lte(options.composeProb, 1);
        }

        options.backgroundWorkloads = options.backgroundWorkloads || [];
        assert(Array.isArray(options.backgroundWorkloads),
               'expected backgroundWorkloads to be an array');

        if (typeof options.dbNamePrefix !== 'undefined') {
            assert.eq(
                'string', typeof options.dbNamePrefix, 'expected dbNamePrefix to be a string');
        }

        options.iterationMultiplier = options.iterationMultiplier || 1;
        assert(Number.isInteger(options.iterationMultiplier),
               'expected iterationMultiplier to be an integer');
        assert.gte(options.iterationMultiplier,
                   1,
                   'expected iterationMultiplier to be greater than or equal to 1');

        options.threadMultiplier = options.threadMultiplier || 1;
        assert(Number.isInteger(options.threadMultiplier),
               'expected threadMultiplier to be an integer');
        assert.gte(options.threadMultiplier,
                   1,
                   'expected threadMultiplier to be greater than or equal to 1');

        return options;
    }

    function validateCleanupOptions(options) {
        var allowedKeys = ['dropDatabaseBlacklist', 'keepExistingDatabases'];

        Object.keys(options).forEach(function(option) {
            assert.contains(option,
                            allowedKeys,
                            'invalid option: ' + tojson(option) + '; valid options are: ' +
                                tojson(allowedKeys));
        });

        if (typeof options.dropDatabaseBlacklist !== 'undefined') {
            assert(Array.isArray(options.dropDatabaseBlacklist),
                   'expected dropDatabaseBlacklist to be an array');
        }

        if (typeof options.keepExistingDatabases !== 'undefined') {
            assert.eq('boolean',
                      typeof options.keepExistingDatabases,
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
        if (executionMode.serial) {
            return Array.shuffle(workloads).map(function(workload) {
                return [workload];  // run each workload by itself
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

        workloads = workloads.slice(0);  // copy
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
            if (clusterOptions.sameCollection && cluster.isSharded() &&
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
                    var shardKey = context[workload].config.data.shardKey || {_id: 'hashed'};
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

    function WorkloadFailure(err, stack, tid, header) {
        this.err = err;
        this.stack = stack;
        this.tid = tid;
        this.header = header;

        this.format = function format() {
            return this.header + '\n' + this.err + '\n\n' + this.stack;
        };
    }

    function throwError(workerErrs) {
        // Returns an array containing all unique values from the stackTraces array,
        // their corresponding number of occurrences in the stackTraces array, and
        // the associated thread ids (tids).
        function freqCount(stackTraces, tids) {
            var uniqueStackTraces = [];
            var associatedTids = [];

            stackTraces.forEach(function(item, stackTraceIndex) {
                var i = uniqueStackTraces.indexOf(item);
                if (i < 0) {
                    uniqueStackTraces.push(item);
                    associatedTids.push(new Set([tids[stackTraceIndex]]));
                } else {
                    associatedTids[i].add(tids[stackTraceIndex]);
                }
            });

            return uniqueStackTraces.map(function(value, i) {
                return {
                    value: value,
                    freq: associatedTids[i].size,
                    tids: Array.from(associatedTids[i])
                };
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

        function prepareMsg(workerErrs) {
            var stackTraces = workerErrs.map(e => e.format());
            var stackTids = workerErrs.map(e => e.tid);
            var uniqueTraces = freqCount(stackTraces, stackTids);
            var numUniqueTraces = uniqueTraces.length;

            // Special case message when threads all have the same trace
            if (numUniqueTraces === 1) {
                return pluralize('thread', stackTraces.length) + ' threw\n\n' +
                    indent(uniqueTraces[0].value, 8);
            }

            var summary = pluralize('exception', stackTraces.length) + ' were thrown, ' +
                numUniqueTraces + ' of which were unique:\n\n';

            return summary +
                uniqueTraces
                    .map(function(obj) {
                        var line = pluralize('thread', obj.freq) + ' with tids ' +
                            JSON.stringify(obj.tids) + ' threw\n';
                        return indent(line + obj.value, 8);
                    })
                    .join('\n\n');
        }

        if (workerErrs.length > 0) {
            var err = new Error(prepareMsg(workerErrs) + '\n');

            // Avoid having any stack traces omitted from the logs
            var maxLogLine = 10 * 1024;  // 10KB

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

    function setIterations(config) {
        // This property must be enumerable because of SERVER-21338, which prevents
        // objects with non-enumerable properties from being serialized properly in
        // ScopedThreads.
        Object.defineProperty(
            config.data, 'iterations', {enumerable: true, value: config.iterations});
    }

    function setThreadCount(config) {
        // This property must be enumerable because of SERVER-21338, which prevents
        // objects with non-enumerable properties from being serialized properly in
        // ScopedThreads.
        Object.defineProperty(
            config.data, 'threadCount', {enumerable: true, value: config.threadCount});
    }

    function useDropDistLockFailPoint(cluster, clusterOptions) {
        assert(cluster.isSharded(), 'cluster is not sharded');

        // For sharded clusters, enable a fail point that allows dropCollection to wait longer
        // to acquire the distributed lock. This prevents tests from failing if the distributed
        // lock is already held by the balancer or by a workload operation. The increased wait
        // is shorter than the distributed-lock-takeover period because otherwise the node
        // would be assumed to be down and the lock would be overtaken.
        clusterOptions.setupFunctions.mongos.push(increaseDropDistLockTimeout);
        clusterOptions.teardownFunctions.mongos.push(resetDropDistLockTimeout);
    }

    function loadWorkloadContext(workloads, context, executionOptions, applyMultipliers) {
        workloads.forEach(function(workload) {
            load(workload);  // for $config
            assert.neq('undefined', typeof $config, '$config was not defined by ' + workload);
            context[workload] = {config: parseConfig($config)};
            if (applyMultipliers) {
                context[workload].config.iterations *= executionOptions.iterationMultiplier;
                context[workload].config.threadCount *= executionOptions.threadMultiplier;
            }
        });
    }

    function printWorkloadSchedule(schedule, backgroundWorkloads) {
        // Print out the entire schedule of workloads to make it easier to run the same
        // schedule when debugging test failures.
        jsTest.log('The entire schedule of FSM workloads:');

        // Note: We use printjsononeline (instead of just plain printjson) to make it
        // easier to reuse the output in variable assignments.
        printjsononeline(schedule);
        if (backgroundWorkloads.length > 0) {
            jsTest.log('Background Workloads:');
            printjsononeline(backgroundWorkloads);
        }

        jsTest.log('End of schedule');
    }

    function cleanupWorkload(
        workload, context, cluster, errors, header, dbHashBlacklist, ttlIndexExists) {
        // Returns true if the workload's teardown succeeds and false if the workload's
        // teardown fails.

        var phase = 'before workload ' + workload + ' teardown';

        try {
            // Ensure that all data has replicated correctly to the secondaries before calling the
            // workload's teardown method.
            cluster.checkReplicationConsistency(dbHashBlacklist, phase, ttlIndexExists);
        } catch (e) {
            errors.push(new WorkloadFailure(
                e.toString(), e.stack, 'main', header + ' checking consistency on secondaries'));
            return false;
        }

        try {
            cluster.validateAllCollections(phase);
        } catch (e) {
            errors.push(new WorkloadFailure(
                e.toString(), e.stack, 'main', header + ' validating collections'));
            return false;
        }

        try {
            teardownWorkload(workload, context, cluster);
        } catch (e) {
            errors.push(new WorkloadFailure(e.toString(), e.stack, 'main', header + ' Teardown'));
            return false;
        }
        return true;
    }

    function recordConfigServerData(cluster, workloads, configServerData, errors) {
        const CONFIG_DATA_LENGTH = 3;

        if (cluster.isSharded()) {
            var newData;
            try {
                newData = cluster.recordAllConfigServerData();
            } catch (e) {
                var failureType = 'Config Server Data Collection';
                errors.push(new WorkloadFailure(e.toString(), e.stack, 'main', failureType));
                return;
            }

            newData.previousWorkloads = workloads;
            newData.time = (new Date()).toISOString();
            configServerData.push(newData);

            // Limit the amount of data recorded to avoid logging too much info when a test
            // fails.
            while (configServerData.length > CONFIG_DATA_LENGTH) {
                configServerData.shift();
            }
        }
    }

    function runWorkloadGroup(threadMgr,
                              workloads,
                              context,
                              cluster,
                              clusterOptions,
                              executionMode,
                              executionOptions,
                              errors,
                              maxAllowedThreads,
                              dbHashBlacklist,
                              configServerData) {
        var cleanup = [];
        var teardownFailed = false;
        var startTime = Date.now();  // Initialize in case setupWorkload fails below.
        var totalTime;
        var ttlIndexExists;

        jsTest.log('Workload(s) started: ' + workloads.join(' '));

        prepareCollections(workloads, context, cluster, clusterOptions, executionOptions);

        try {
            // Set up the thread manager for this set of foreground workloads.
            startTime = Date.now();
            threadMgr.init(workloads, context, maxAllowedThreads);

            // Call each foreground workload's setup function.
            workloads.forEach(function(workload) {
                // Define "iterations" and "threadCount" properties on the foreground workload's
                // $config.data object so that they can be used within its setup(), teardown(), and
                // state functions. This must happen after calling threadMgr.init() in case the
                // thread counts needed to be scaled down.
                setIterations(context[workload].config);
                setThreadCount(context[workload].config);

                setupWorkload(workload, context, cluster);
                cleanup.push(workload);
            });

            try {
                // Start this set of foreground workload threads.
                threadMgr.spawnAll(cluster, executionOptions);
                // Allow 20% of foreground threads to fail. This allows the workloads to run on
                // underpowered test hosts.
                threadMgr.checkFailed(0.2);
            } finally {
                // Threads must be joined before destruction, so do this
                // even in the presence of exceptions.
                errors.push(...threadMgr.joinAll().map(
                    e => new WorkloadFailure(
                        e.err, e.stack, e.tid, 'Foreground ' + e.workloads.join(' '))));
            }
        } finally {
            // Checking that the data is consistent across the primary and secondaries requires
            // additional complexity to prevent writes from occurring in the background on the
            // primary due to the TTL monitor. If none of the workloads actually created any TTL
            // indexes (and we dropped the data of any previous workloads), then don't expend any
            // additional effort in trying to handle that case.
            ttlIndexExists =
                workloads.some(workload => context[workload].config.data.ttlIndexExists);

            // Call each foreground workload's teardown function. After all teardowns have completed
            // check if any of them failed.
            var cleanupResults = cleanup.map(workload => cleanupWorkload(workload,
                                                                         context,
                                                                         cluster,
                                                                         errors,
                                                                         'Foreground',
                                                                         dbHashBlacklist,
                                                                         ttlIndexExists));
            teardownFailed = cleanupResults.some(success => (success === false));

            totalTime = Date.now() - startTime;
            jsTest.log('Workload(s) completed in ' + totalTime + ' ms: ' + workloads.join(' '));

            recordConfigServerData(cluster, workloads, configServerData, errors);
        }

        // Only drop the collections/databases if all the workloads ran successfully.
        if (!errors.length && !teardownFailed) {
            cleanupWorkloadData(workloads, context, clusterOptions);
        }

        // Throw any existing errors so that the schedule aborts.
        throwError(errors);

        // All workload data should have been dropped at this point, so there shouldn't be any TTL
        // indexes.
        ttlIndexExists = false;

        // Ensure that all operations replicated correctly to the secondaries.
        cluster.checkReplicationConsistency(
            dbHashBlacklist, 'after workload-group teardown and data clean-up', ttlIndexExists);
    }

    function runWorkloads(
        workloads, clusterOptions, executionMode, executionOptions, cleanupOptions) {
        assert.gt(workloads.length, 0, 'need at least one workload to run');

        executionMode = validateExecutionMode(executionMode);
        Object.freeze(executionMode);  // immutable after validation (and normalization)

        validateExecutionOptions(executionMode, executionOptions);
        Object.freeze(executionOptions);  // immutable after validation (and normalization)

        Object.freeze(cleanupOptions);  // immutable prior to validation
        validateCleanupOptions(cleanupOptions);

        if (executionMode.composed) {
            clusterOptions.sameDB = true;
            clusterOptions.sameCollection = true;
        }

        // Determine how strong to make assertions while simultaneously executing
        // different workloads.
        var assertLevel = AssertLevel.OWN_DB;
        if (clusterOptions.sameDB) {
            // The database is shared by multiple workloads, so only make the asserts
            // that apply when the collection is owned by an individual workload.
            assertLevel = AssertLevel.OWN_COLL;
        }
        if (clusterOptions.sameCollection) {
            // The collection is shared by multiple workloads, so only make the asserts
            // that always apply.
            assertLevel = AssertLevel.ALWAYS;
        }
        globalAssertLevel = assertLevel;

        var context = {};
        loadWorkloadContext(workloads, context, executionOptions, true /* applyMultipliers */);
        var threadMgr = new ThreadManager(clusterOptions, executionMode);

        var bgContext = {};
        var bgWorkloads = executionOptions.backgroundWorkloads;
        loadWorkloadContext(bgWorkloads, bgContext, executionOptions, false /* applyMultipliers */);
        var bgThreadMgr = new ThreadManager(clusterOptions);

        var cluster = new Cluster(clusterOptions);
        if (cluster.isSharded()) {
            useDropDistLockFailPoint(cluster, clusterOptions);
        }
        cluster.setup();

        // Clean up the state left behind by other tests in the concurrency suite
        // to avoid having too many open files.

        // List of DBs that will not be dropped.
        var dbBlacklist = ['admin', 'config', 'local', '$external'];

        // List of DBs that dbHash is not run on.
        var dbHashBlacklist = ['local'];

        if (cleanupOptions.dropDatabaseBlacklist) {
            dbBlacklist.push(...cleanupOptions.dropDatabaseBlacklist);
            dbHashBlacklist.push(...cleanupOptions.dropDatabaseBlacklist);
        }
        if (!cleanupOptions.keepExistingDatabases) {
            dropAllDatabases(cluster.getDB('test'), dbBlacklist);
        }

        var maxAllowedThreads = 100 * executionOptions.threadMultiplier;
        Random.setRandomSeed(clusterOptions.seed);
        var bgCleanup = [];
        var errors = [];
        var configServerData = [];

        try {
            prepareCollections(bgWorkloads, bgContext, cluster, clusterOptions, executionOptions);

            // Set up the background thread manager for background workloads.
            bgThreadMgr.init(bgWorkloads, bgContext, maxAllowedThreads);

            // Call each background workload's setup function.
            bgWorkloads.forEach(function(bgWorkload) {
                // Define "iterations" and "threadCount" properties on the background workload's
                // $config.data object so that they can be used within its setup(), teardown(), and
                // state functions. This must happen after calling bgThreadMgr.init() in case the
                // thread counts needed to be scaled down.
                setIterations(bgContext[bgWorkload].config);
                setThreadCount(bgContext[bgWorkload].config);

                setupWorkload(bgWorkload, bgContext, cluster);
                bgCleanup.push(bgWorkload);
            });

            try {
                // Start background workload threads.
                bgThreadMgr.spawnAll(cluster, executionOptions);
                bgThreadMgr.checkFailed(0);

                var schedule = scheduleWorkloads(workloads, executionMode, executionOptions);
                printWorkloadSchedule(schedule, bgWorkloads);

                schedule.forEach(function(workloads) {
                    // Check if any background workloads have failed.
                    if (bgThreadMgr.checkForErrors()) {
                        var msg = 'Background workload failed before all foreground workloads ran';
                        throw new IterationEnd(msg);
                    }

                    // Make a deep copy of the $config object for each of the workloads that are
                    // going to be run to ensure the workload starts with a fresh version of its
                    // $config.data. This is necessary because $config.data keeps track of
                    // thread-local state that may be updated during a workload's setup(),
                    // teardown(), and state functions.
                    var groupContext = {};
                    workloads.forEach(function(workload) {
                        groupContext[workload] = Object.extend({}, context[workload], true);
                    });

                    // Run the next group of workloads in the schedule.
                    runWorkloadGroup(threadMgr,
                                     workloads,
                                     groupContext,
                                     cluster,
                                     clusterOptions,
                                     executionMode,
                                     executionOptions,
                                     errors,
                                     maxAllowedThreads,
                                     dbHashBlacklist,
                                     configServerData);
                });
            } finally {
                // Set a flag so background threads know to terminate.
                bgThreadMgr.markAllForTermination();
                errors.push(...bgThreadMgr.joinAll().map(
                    e => new WorkloadFailure(
                        e.err, e.stack, e.tid, 'Background ' + e.workloads.join(' '))));
            }
        } finally {
            try {
                // Checking that the data is consistent across the primary and secondaries requires
                // additional complexity to prevent writes from occurring in the background on the
                // primary due to the TTL monitor. If none of the workloads actually created any TTL
                // indexes (and we dropped the data of any previous workloads), then don't expend
                // any additional effort in trying to handle that case.
                var ttlIndexExists = bgWorkloads.some(
                    bgWorkload => bgContext[bgWorkload].config.data.ttlIndexExists);

                // Call each background workload's teardown function.
                bgCleanup.forEach(bgWorkload => cleanupWorkload(bgWorkload,
                                                                bgContext,
                                                                cluster,
                                                                errors,
                                                                'Background',
                                                                dbHashBlacklist,
                                                                ttlIndexExists));
                // TODO: Call cleanupWorkloadData() on background workloads here if no background
                // workload teardown functions fail.

                // Replace the active exception with an exception describing the errors from all
                // the foreground and background workloads. IterationEnd errors are ignored because
                // they are thrown when the background workloads are instructed by the thread
                // manager to terminate.
                var workloadErrors = errors.filter(e => !e.err.startsWith('IterationEnd:'));

                if (cluster.isSharded() && workloadErrors.length) {
                    jsTest.log('Config Server Data:\n' + tojsononeline(configServerData));
                }

                throwError(workloadErrors);
            } finally {
                cluster.teardown();
            }
        }
    }

    return {
        serial: function serial(workloads, clusterOptions, executionOptions, cleanupOptions) {
            clusterOptions = clusterOptions || {};
            executionOptions = executionOptions || {};
            cleanupOptions = cleanupOptions || {};

            runWorkloads(
                workloads, clusterOptions, {serial: true}, executionOptions, cleanupOptions);
        },

        parallel: function parallel(workloads, clusterOptions, executionOptions, cleanupOptions) {
            clusterOptions = clusterOptions || {};
            executionOptions = executionOptions || {};
            cleanupOptions = cleanupOptions || {};

            runWorkloads(
                workloads, clusterOptions, {parallel: true}, executionOptions, cleanupOptions);
        },

        composed: function composed(workloads, clusterOptions, executionOptions, cleanupOptions) {
            clusterOptions = clusterOptions || {};
            executionOptions = executionOptions || {};
            cleanupOptions = cleanupOptions || {};

            runWorkloads(
                workloads, clusterOptions, {composed: true}, executionOptions, cleanupOptions);
        }
    };

})();

var runWorkloadsSerially = runner.serial;
var runWorkloadsInParallel = runner.parallel;
var runCompositionOfWorkloads = runner.composed;
