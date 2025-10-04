import {Cluster} from "jstests/concurrency/fsm_libs/cluster.js";
import {parseConfig} from "jstests/concurrency/fsm_libs/parse_config.js";
import {ThreadManager} from "jstests/concurrency/fsm_libs/thread_mgr.js";
import {uniqueCollName, uniqueDBName} from "jstests/concurrency/fsm_utils/name_utils.js";
import {ShardTransitionUtil} from "jstests/libs/shard_transition_util.js";

export const runner = (function () {
    function validateExecutionMode(mode) {
        let allowedKeys = ["parallel", "serial"];

        Object.keys(mode).forEach(function (option) {
            assert.contains(
                option,
                allowedKeys,
                "invalid option: " + tojson(option) + "; valid options are: " + tojson(allowedKeys),
            );
        });

        mode.parallel = mode.parallel || false;
        assert.eq("boolean", typeof mode.parallel);

        mode.serial = mode.serial || false;
        assert.eq("boolean", typeof mode.serial);

        let numEnabledModes = 0;
        Object.keys(mode).forEach((key) => {
            if (mode[key]) {
                numEnabledModes++;
            }
        });
        assert.eq(1, numEnabledModes, "One and only one execution mode can be enabled " + tojson(mode));

        return mode;
    }

    function validateExecutionOptions(mode, options) {
        let allowedKeys = [
            "dbNamePrefix",
            "iterationMultiplier",
            "sessionOptions",
            "actionFiles",
            "threadMultiplier",
            "tenantId",
        ];

        if (mode.parallel) {
            allowedKeys.push("numSubsets");
            allowedKeys.push("subsetSize");
        }

        Object.keys(options).forEach(function (option) {
            assert.contains(
                option,
                allowedKeys,
                "invalid option: " + tojson(option) + "; valid options are: " + tojson(allowedKeys),
            );
        });

        if (typeof options.subsetSize !== "undefined") {
            assert(Number.isInteger(options.subsetSize), "expected subset size to be an integer");
            assert.gt(options.subsetSize, 1);
        }

        if (typeof options.numSubsets !== "undefined") {
            assert(Number.isInteger(options.numSubsets), "expected number of subsets to be an integer");
            assert.gt(options.numSubsets, 0);
        }

        if (typeof options.iterations !== "undefined") {
            assert(Number.isInteger(options.iterations), "expected number of iterations to be an integer");
            assert.gt(options.iterations, 0);
        }

        if (typeof options.dbNamePrefix !== "undefined") {
            assert.eq("string", typeof options.dbNamePrefix, "expected dbNamePrefix to be a string");
        }

        options.iterationMultiplier = options.iterationMultiplier || 1;
        assert(Number.isInteger(options.iterationMultiplier), "expected iterationMultiplier to be an integer");
        assert.gte(options.iterationMultiplier, 1, "expected iterationMultiplier to be greater than or equal to 1");

        if (typeof options.actionFiles !== "undefined") {
            assert.eq("string", typeof options.actionFiles.permitted, "expected actionFiles.permitted to be a string");

            assert.eq(
                "string",
                typeof options.actionFiles.idleRequest,
                "expected actionFiles.idleRequest to be a string",
            );

            assert.eq("string", typeof options.actionFiles.idleAck, "expected actionFiles.idleAck to be a string");
        }

        options.threadMultiplier = options.threadMultiplier || 1;
        assert(Number.isInteger(options.threadMultiplier), "expected threadMultiplier to be an integer");
        assert.gte(options.threadMultiplier, 1, "expected threadMultiplier to be greater than or equal to 1");

        return options;
    }

    function validateCleanupOptions(options) {
        let allowedKeys = ["dropDatabaseDenylist", "keepExistingDatabases", "validateCollections"];

        Object.keys(options).forEach(function (option) {
            assert.contains(
                option,
                allowedKeys,
                "invalid option: " + tojson(option) + "; valid options are: " + tojson(allowedKeys),
            );
        });

        if (typeof options.dropDatabaseDenylist !== "undefined") {
            assert(Array.isArray(options.dropDatabaseDenylist), "expected dropDatabaseDenylist to be an array");
        }

        if (typeof options.keepExistingDatabases !== "undefined") {
            assert.eq(
                "boolean",
                typeof options.keepExistingDatabases,
                "expected keepExistingDatabases to be a boolean",
            );
        }

        options.validateCollections = options.hasOwnProperty("validateCollections")
            ? options.validateCollections
            : true;
        assert.eq("boolean", typeof options.validateCollections, "expected validateCollections to be a boolean");

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
            return Array.shuffle(workloads).map(function (workload) {
                return [workload]; // run each workload by itself
            });
        }

        let schedule = [];

        // Take 'numSubsets' random subsets of the workloads, each
        // of size 'subsetSize'. Each workload must get scheduled
        // once before any workload can be scheduled again.
        let subsetSize = executionOptions.subsetSize || 10;

        // If the number of subsets is not specified, then have each
        // workload get scheduled 2 to 3 times.
        let numSubsets = executionOptions.numSubsets;
        if (!numSubsets) {
            numSubsets = Math.ceil((2.5 * workloads.length) / subsetSize);
        }

        workloads = workloads.slice(0); // copy
        workloads = Array.shuffle(workloads);

        let start = 0;
        let end = subsetSize;

        while (schedule.length < numSubsets) {
            schedule.push(workloads.slice(start, end));

            start = end;
            end += subsetSize;

            // Check if there are not enough elements remaining in
            // 'workloads' to make a subset of size 'subsetSize'.
            if (end > workloads.length) {
                // Re-shuffle the beginning of the array, and prepend it
                // with the workloads that have not been scheduled yet.
                let temp = Array.shuffle(workloads.slice(0, start));
                for (let i = workloads.length - 1; i >= start; --i) {
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
        let dbName, collName, myDB;
        let firstWorkload = true;

        workloads.forEach(function (workload) {
            // Workloads cannot have a shardKey if sameCollection is specified
            if (clusterOptions.sameCollection && cluster.isSharded() && context[workload].config.data.shardKey) {
                throw new Error("cannot specify a shardKey with sameCollection option");
            }
            if (firstWorkload || !clusterOptions.sameCollection) {
                if (firstWorkload || !clusterOptions.sameDB) {
                    dbName = uniqueDBName(executionOptions.dbNamePrefix);
                }
                collName = uniqueCollName();
                myDB = cluster.getDB(dbName);
                myDB[collName].drop();

                if (cluster.isSharded()) {
                    // If the suite specifies shardCollection probability, only shard this
                    // collection with that probability unless the workload expects it to be sharded
                    // (i.e. specified a custom shard key).
                    const shouldShard =
                        typeof context[workload].config.data.shardKey !== "undefined" ||
                        typeof TestData.shardCollectionProbability == "undefined" ||
                        Math.random() < TestData.shardCollectionProbability;
                    print(
                        "Preparing test collection " +
                            tojsononeline({
                                dbName,
                                collName,
                                customShardKey: context[workload].config.data.shardKey,
                                shardCollectionProbability: TestData.shardCollectionProbability,
                                shouldShard,
                            }),
                    );
                    if (shouldShard) {
                        let shardKey = context[workload].config.data.shardKey || {_id: "hashed"};
                        // TODO: allow workload config data to specify split
                        cluster.shardCollection(myDB[collName], shardKey, false);
                    }
                }
            }

            context[workload].db = myDB;
            context[workload].dbName = dbName;
            context[workload].collName = collName;

            firstWorkload = false;
        });
    }

    function dropAllDatabases(db, denylist) {
        let res = db.adminCommand("listDatabases");
        assert.commandWorked(res);

        res.databases.forEach(function (dbInfo) {
            if (!Array.contains(denylist, dbInfo.name)) {
                assert.commandWorked(db.getSiblingDB(dbInfo.name).dropDatabase());
            }
        });
    }

    function cleanupWorkloadData(workloads, context, clusterOptions) {
        // If no other workloads will be using this collection,
        // then drop it to avoid having too many files open
        if (!clusterOptions.sameCollection) {
            workloads.forEach(function (workload) {
                let config = context[workload];
                config.db[config.collName].drop();
            });
        }

        // If no other workloads will be using this database,
        // then drop it to avoid having too many files open
        if (!clusterOptions.sameDB) {
            workloads.forEach(function (workload) {
                let config = context[workload];
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
            return this.header + "\n" + this.err + "\n\n" + this.stack;
        };
    }

    function throwError(workerErrs) {
        // Returns an array containing all unique values from the stackTraces array,
        // their corresponding number of occurrences in the stackTraces array, and
        // the associated thread ids (tids).
        function freqCount(stackTraces, tids) {
            let uniqueStackTraces = [];
            let associatedTids = [];

            stackTraces.forEach(function (item, stackTraceIndex) {
                let i = uniqueStackTraces.indexOf(item);
                if (i < 0) {
                    uniqueStackTraces.push(item);
                    associatedTids.push(new Set([tids[stackTraceIndex]]));
                } else {
                    associatedTids[i].add(tids[stackTraceIndex]);
                }
            });

            return uniqueStackTraces.map(function (value, i) {
                return {
                    value: value,
                    freq: associatedTids[i].size,
                    tids: Array.from(associatedTids[i]),
                };
            });
        }

        // Indents a multiline string with the specified number of spaces.
        function indent(str, size) {
            const prefix = " ".repeat(size);
            return prefix + str.split("\n").join("\n" + prefix);
        }

        function pluralize(str, num) {
            let suffix = num > 1 ? "s" : "";
            return num + " " + str + suffix;
        }

        function prepareMsg(workerErrs) {
            let stackTraces = workerErrs.map((e) => e.format());
            let stackTids = workerErrs.map((e) => e.tid);
            let uniqueTraces = freqCount(stackTraces, stackTids);
            let numUniqueTraces = uniqueTraces.length;

            // Special case message when threads all have the same trace
            if (numUniqueTraces === 1) {
                return (
                    pluralize("thread", stackTraces.length) +
                    " with tids " +
                    JSON.stringify(stackTids) +
                    " threw\n\n" +
                    indent(uniqueTraces[0].value, 8)
                );
            }

            let summary =
                pluralize("exception", stackTraces.length) +
                " were thrown, " +
                numUniqueTraces +
                " of which were unique:\n\n";

            return (
                summary +
                uniqueTraces
                    .map(function (obj) {
                        let line =
                            pluralize("thread", obj.freq) + " with tids " + JSON.stringify(obj.tids) + " threw\n";
                        return indent(line + obj.value, 8);
                    })
                    .join("\n\n")
            );
        }

        if (workerErrs.length > 0) {
            let err = new Error(prepareMsg(workerErrs) + "\n");

            // Avoid having any stack traces omitted from the logs
            let maxLogLine = 10 * 1024; // 10KB

            // Check if the combined length of the error message and the stack traces
            // exceeds the maximum line-length the shell will log.
            if (err.message.length + err.stack.length >= maxLogLine) {
                print(err.message);
                print(err.stack);
                throw new Error("stack traces would have been snipped, see logs");
            }

            throw err;
        }
    }

    function setupWorkload(workload, context, cluster) {
        let myDB = context[workload].db;
        let collName = context[workload].collName;

        const fn = () => {
            let config = context[workload].config;
            config.setup.call(config.data, myDB, collName, cluster);
        };

        if (TestData.shardsAddedRemoved) {
            ShardTransitionUtil.retryOnShardTransitionErrors(fn);
        } else {
            fn();
        }
    }

    function teardownWorkload(workload, context, cluster) {
        let myDB = context[workload].db;
        let collName = context[workload].collName;

        let config = context[workload].config;
        config.teardown.call(config.data, myDB, collName, cluster);
    }

    function setIterations(config) {
        // This property must be enumerable because of SERVER-21338, which prevents
        // objects with non-enumerable properties from being serialized properly in
        // Threads.
        Object.defineProperty(config.data, "iterations", {enumerable: true, value: config.iterations});
    }

    function setThreadCount(config) {
        // This property must be enumerable because of SERVER-21338, which prevents
        // objects with non-enumerable properties from being serialized properly in
        // Threads.
        Object.defineProperty(config.data, "threadCount", {enumerable: true, value: config.threadCount});
    }

    async function loadWorkloadContext(workloads, context, executionOptions, applyMultipliers) {
        for (const workload of workloads) {
            print(`Loading FSM workload: ${workload}`);
            const {$config} = await import(workload);
            assert.neq("undefined", typeof $config, "$config was not defined by " + workload);
            context[workload] = {config: parseConfig($config)};
            if (applyMultipliers) {
                context[workload].config.iterations *= executionOptions.iterationMultiplier;
                context[workload].config.threadCount *= executionOptions.threadMultiplier;
            }
        }
    }

    function printWorkloadSchedule(schedule) {
        // Print out the entire schedule of workloads to make it easier to run the same
        // schedule when debugging test failures.
        jsTest.log("The entire schedule of FSM workloads:");

        // Note: We use printjsononeline (instead of just plain printjson) to make it
        // easier to reuse the output in variable assignments.
        printjsononeline(schedule);

        jsTest.log("End of schedule");
    }

    function cleanupWorkload(workload, context, cluster, errors, header, dbHashDenylist, cleanupOptions) {
        // Returns true if the workload's teardown succeeds and false if the workload's
        // teardown fails.

        let phase = "before workload " + workload + " teardown";

        try {
            // Ensure that all data has replicated correctly to the secondaries before calling the
            // workload's teardown method.
            cluster.checkReplicationConsistency(dbHashDenylist, phase);
        } catch (e) {
            errors.push(
                new WorkloadFailure(e.toString(), e.stack, "main", header + " checking consistency on secondaries"),
            );
            return false;
        }

        try {
            if (cleanupOptions.validateCollections) {
                cluster.validateAllCollections(phase);
            }
        } catch (e) {
            errors.push(new WorkloadFailure(e.toString(), e.stack, "main", header + " validating collections"));
            return false;
        }

        try {
            teardownWorkload(workload, context, cluster);
        } catch (e) {
            errors.push(new WorkloadFailure(e.toString(), e.stack, "main", header + " Teardown"));
            return false;
        }
        return true;
    }

    function runWorkloadGroup(
        threadMgr,
        workloads,
        context,
        cluster,
        clusterOptions,
        executionMode,
        executionOptions,
        errors,
        maxAllowedThreads,
        dbHashDenylist,
        cleanupOptions,
    ) {
        let cleanup = [];
        let teardownFailed = false;
        let startTime = Date.now(); // Initialize in case setupWorkload fails below.
        let totalTime;

        jsTest.log("Workload(s) started: " + workloads.join(" "));

        prepareCollections(workloads, context, cluster, clusterOptions, executionOptions);

        try {
            // Overrides for main thread's execution of fsm_workload setup functions
            if (typeof TestData.fsmPreOverridesLoadedCallback !== "undefined") {
                new Function(`${TestData.fsmPreOverridesLoadedCallback}`)();
            }

            // Set up the thread manager for this set of foreground workloads.
            startTime = Date.now();
            threadMgr.init(workloads, context, maxAllowedThreads);

            // Call each foreground workload's setup function.
            workloads.forEach(function (workload) {
                // Define "iterations" and "threadCount" properties on the foreground workload's
                // $config.data object so that they can be used within its setup(), teardown(), and
                // state functions. This must happen after calling threadMgr.init() in case the
                // thread counts needed to be scaled down.
                setIterations(context[workload].config);
                setThreadCount(context[workload].config);

                setupWorkload(workload, context, cluster);
                cleanup.push(workload);
            });

            // Since the worker threads may be running with causal consistency enabled, we set the
            // initial clusterTime and initial operationTime for the sessions they'll create so that
            // they are guaranteed to observe the effects of the workload's $config.setup() function
            // being called.
            if (typeof executionOptions.sessionOptions === "object" && executionOptions.sessionOptions !== null) {
                // We only start a session for the worker threads and never start one for the main
                // thread. We can therefore get the clusterTime and operationTime tracked by the
                // underlying DummyDriverSession through any DB instance (i.e. the "test" database
                // here was chosen arbitrarily).
                const session = cluster.getDB("test").getSession();

                // JavaScript objects backed by C++ objects (e.g. BSON values from a command
                // response) do not serialize correctly when passed through the Thread
                // constructor. To work around this behavior, we instead pass a stringified form of
                // the JavaScript object through the Thread constructor and use eval() to
                // rehydrate it.
                executionOptions.sessionOptions.initialClusterTime = tojson(session.getClusterTime());
                executionOptions.sessionOptions.initialOperationTime = tojson(session.getOperationTime());
            }

            try {
                // Start this set of foreground workload threads.
                threadMgr.spawnAll(cluster, executionOptions);
                // Allow 20% of foreground threads to fail. This allows the workloads to run on
                // underpowered test hosts.
                threadMgr.checkFailed(0.2);
            } finally {
                // Threads must be joined before destruction, so do this
                // even in the presence of exceptions.
                errors.push(
                    ...threadMgr
                        .joinAll()
                        .map((e) => new WorkloadFailure(e.err, e.stack, e.tid, "Foreground " + e.workloads.join(" "))),
                );
            }
        } finally {
            // Call each foreground workload's teardown function. After all teardowns have completed
            // check if any of them failed.
            let cleanupResults = cleanup.map((workload) =>
                cleanupWorkload(workload, context, cluster, errors, "Foreground", dbHashDenylist, cleanupOptions),
            );
            teardownFailed = cleanupResults.some((success) => success === false);

            totalTime = Date.now() - startTime;
            jsTest.log("Workload(s) completed in " + totalTime + " ms: " + workloads.join(" "));
        }

        // Only drop the collections/databases if all the workloads ran successfully.
        if (!errors.length && !teardownFailed) {
            cleanupWorkloadData(workloads, context, clusterOptions);
        }

        // Throw any existing errors so that the schedule aborts.
        throwError(errors);

        // Ensure that all operations replicated correctly to the secondaries.
        cluster.checkReplicationConsistency(dbHashDenylist, "after workload-group teardown and data clean-up");
    }

    async function runWorkloads(workloads, clusterOptions, executionMode, executionOptions, cleanupOptions) {
        assert.gt(workloads.length, 0, "need at least one workload to run");

        executionMode = validateExecutionMode(executionMode);
        Object.freeze(executionMode); // immutable after validation (and normalization)

        validateExecutionOptions(executionMode, executionOptions);
        Object.freeze(executionOptions); // immutable after validation (and normalization)

        validateCleanupOptions(cleanupOptions);
        Object.freeze(cleanupOptions); // immutable after validation (and normalization)

        let context = {};
        await loadWorkloadContext(workloads, context, executionOptions, true /* applyMultipliers */);
        let threadMgr = new ThreadManager(clusterOptions);

        let cluster = new Cluster(clusterOptions, executionOptions.sessionOptions);
        cluster.setup();

        // Clean up the state left behind by other tests in the concurrency suite
        // to avoid having too many open files.

        // List of DBs that will not be dropped.
        let dbDenylist = ["admin", "config", "local", "$external"];

        // List of DBs that dbHash is not run on.
        let dbHashDenylist = ["local"];

        if (cleanupOptions.dropDatabaseDenylist) {
            dbDenylist.push(...cleanupOptions.dropDatabaseDenylist);
            dbHashDenylist.push(...cleanupOptions.dropDatabaseDenylist);
        }
        if (!cleanupOptions.keepExistingDatabases) {
            dropAllDatabases(cluster.getDB("test"), dbDenylist);
        }

        let maxAllowedThreads = 100 * executionOptions.threadMultiplier;
        Random.setRandomSeed(clusterOptions.seed);
        let errors = [];

        try {
            let schedule = scheduleWorkloads(workloads, executionMode, executionOptions);
            printWorkloadSchedule(schedule);

            schedule.forEach(function (workloads) {
                // Make a deep copy of the $config object for each of the workloads that are
                // going to be run to ensure the workload starts with a fresh version of its
                // $config.data. This is necessary because $config.data keeps track of
                // thread-local state that may be updated during a workload's setup(),
                // teardown(), and state functions.
                let groupContext = {};
                workloads.forEach(function (workload) {
                    groupContext[workload] = Object.extend({}, context[workload], true);
                });

                // Run the next group of workloads in the schedule.
                runWorkloadGroup(
                    threadMgr,
                    workloads,
                    groupContext,
                    cluster,
                    clusterOptions,
                    executionMode,
                    executionOptions,
                    errors,
                    maxAllowedThreads,
                    dbHashDenylist,
                    cleanupOptions,
                );
            });

            throwError(errors);
        } finally {
            cluster.teardown();
        }
    }

    return {
        serial: async function serial(workloads, clusterOptions, executionOptions, cleanupOptions) {
            clusterOptions = clusterOptions || {};
            executionOptions = executionOptions || {};
            cleanupOptions = cleanupOptions || {};

            await runWorkloads(workloads, clusterOptions, {serial: true}, executionOptions, cleanupOptions);
        },

        parallel: async function parallel(workloads, clusterOptions, executionOptions, cleanupOptions) {
            clusterOptions = clusterOptions || {};
            executionOptions = executionOptions || {};
            cleanupOptions = cleanupOptions || {};

            await runWorkloads(workloads, clusterOptions, {parallel: true}, executionOptions, cleanupOptions);
        },

        internals: {
            validateExecutionOptions,
            prepareCollections,
            WorkloadFailure,
            throwError,
            setupWorkload,
            teardownWorkload,
            setIterations,
            setThreadCount,
            loadWorkloadContext,
        },
    };
})();

export const runWorkloadsSerially = runner.serial;
export const runWorkloadsInParallel = runner.parallel;
