import {workerThread} from "jstests/concurrency/fsm_libs/worker_thread.js";
import {Thread} from "jstests/libs/parallelTester.js";

/**
 * Helper for spawning and joining worker threads.
 */

export const ThreadManager = function (clusterOptions) {
    if (!(this instanceof ThreadManager)) {
        return new ThreadManager(clusterOptions);
    }

    function makeThread(workloads, args, options) {
        // Wrap the execution of 'threadFn' in a try/finally block
        // to ensure that the database connection implicitly created
        // within the thread's scope is closed.
        let guardedThreadFn = function (threadFn, workloads, args, options) {
            try {
                return threadFn(workloads, args, options);
            } finally {
                if (typeof db !== "undefined") {
                    globalThis.db = undefined;
                    gc();
                }
            }
        };

        return new Thread(guardedThreadFn, workerThread.fsm, workloads, args, options);
    }

    let latch;
    let errorLatch;
    let numThreads;

    let initialized = false;
    let threads = [];

    let _workloads, _context;

    this.init = function init(workloads, context, maxAllowedThreads) {
        assert.eq("number", typeof maxAllowedThreads, "the maximum allowed threads must be a number");
        assert.gt(maxAllowedThreads, 0, "the maximum allowed threads must be positive");
        assert.eq(maxAllowedThreads, Math.floor(maxAllowedThreads), "the maximum allowed threads must be an integer");

        function computeNumThreads() {
            // If we don't have any workloads, return 0.
            if (workloads.length === 0) {
                return 0;
            }
            return Array.sum(
                workloads.map(function (workload) {
                    return context[workload].config.threadCount;
                }),
            );
        }

        let requestedNumThreads = computeNumThreads();
        let perLoadThreads = {};
        let factor = 1;
        if (requestedNumThreads > maxAllowedThreads) {
            // Scale down the requested '$config.threadCount' values to make
            // them sum to less than 'maxAllowedThreads'
            factor = maxAllowedThreads / requestedNumThreads;
            workloads.forEach(function (workload) {
                let config = context[workload].config;
                let threadCount = config.threadCount;
                threadCount = Math.floor(factor * threadCount);
                threadCount = Math.max(1, threadCount); // ensure workload is executed
                config.threadCount = threadCount;
                perLoadThreads[workload] = threadCount;
            });
        }

        numThreads = computeNumThreads();
        assert.lte(
            numThreads,
            maxAllowedThreads,
            tojson({
                "requestedNumThreads": requestedNumThreads,
                "maxAllowedThreads": maxAllowedThreads,
                "factor": factor,
                "perLoadThreads": perLoadThreads,
            }),
        );
        latch = new CountDownLatch(numThreads);
        errorLatch = new CountDownLatch(numThreads);

        let plural = numThreads === 1 ? "" : "s";
        print("Using " + numThreads + " thread" + plural + " (requested " + requestedNumThreads + ")");

        _workloads = workloads;
        _context = context;

        initialized = true;
    };

    this.spawnAll = function spawnAll(cluster, options) {
        if (!initialized) {
            throw new Error("thread manager has not been initialized yet");
        }

        let workloadData = {};
        let tid = 0;
        _workloads.forEach(function (workload) {
            let config = _context[workload].config;
            workloadData[workload] = config.data;
            let workloads = [workload]; // worker thread only needs to load 'workload'

            for (let i = 0; i < config.threadCount; ++i) {
                let args = {
                    tid: tid++,
                    tenantId: options.tenantId,
                    data: workloadData,
                    host: cluster.getHost(),
                    secondaryHost: cluster.getSecondaryHost(),
                    replSetName: cluster.getReplSetName(),
                    latch: latch,
                    dbName: _context[workload].dbName,
                    collName: _context[workload].collName,
                    cluster: cluster.getSerializedCluster(),
                    clusterOptions: clusterOptions,
                    seed: Random.randInt(1e13), // contains range of Date.getTime()
                    errorLatch: errorLatch,
                    sessionOptions: options.sessionOptions,
                    numThreads: numThreads,
                };

                let t = makeThread(workloads, args, options);
                threads.push(t);
                t.start();
            }
        });
    };

    this.checkFailed = function checkFailed(allowedFailurePercent) {
        if (!initialized) {
            throw new Error("thread manager has not been initialized yet");
        }

        let failedThreadIndexes = [];
        function handleFailedThread(thread, index) {
            if (thread.hasFailed() && !Array.contains(failedThreadIndexes, index)) {
                failedThreadIndexes.push(index);
                latch.countDown();
            }
        }

        while (latch.getCount() > 0) {
            threads.forEach(handleFailedThread);
            sleep(100);
        }

        let failedThreads = failedThreadIndexes.length;
        if (failedThreads > 0) {
            print(failedThreads + " thread(s) threw a JS or C++ exception while spawning");
        }

        if (failedThreads / threads.length > allowedFailurePercent) {
            throw new Error("Too many worker threads failed to spawn - aborting");
        }
    };

    this.checkForErrors = function checkForErrors() {
        if (!initialized) {
            throw new Error("thread manager has not been initialized yet");
        }

        // Each worker thread receives the errorLatch as an argument. The worker thread
        // decreases the count when it receives an error.
        return errorLatch.getCount() < numThreads;
    };

    this.joinAll = function joinAll() {
        if (!initialized) {
            throw new Error("thread manager has not been initialized yet");
        }

        let errors = [];

        threads.forEach(function (t) {
            t.join();

            let data = t.returnData();
            if (data && !data.ok) {
                errors.push(data);
            }
        });

        initialized = false;
        threads = [];

        return errors;
    };
};

/**
 * Extensions to the 'workerThread' module for executing a single FSM-based
 * workload and a composition of them, respectively.
 */

workerThread.fsm = async function (workloads, args, options) {
    const {workerThread} = await import("jstests/concurrency/fsm_libs/worker_thread.js");
    const {fsm} = await import("jstests/concurrency/fsm_libs/fsm.js");

    return workerThread.main(workloads, args, async function (configs) {
        let workloads = Object.keys(configs);
        assert.eq(1, workloads.length);
        await fsm.run(configs[workloads[0]]);
    });
};
