'use strict';

load('jstests/libs/parallelTester.js');                 // for ScopedThread and CountDownLatch
load('jstests/concurrency/fsm_libs/worker_thread.js');  // for workerThread

/**
 * Helper for spawning and joining worker threads.
 */

var ThreadManager = function(clusterOptions, executionMode = {composed: false}) {
    if (!(this instanceof ThreadManager)) {
        return new ThreadManager(clusterOptions, executionMode);
    }

    function makeThread(workloads, args, options) {
        // Wrap the execution of 'threadFn' in a try/finally block
        // to ensure that the database connection implicitly created
        // within the thread's scope is closed.
        var guardedThreadFn = function(threadFn, workloads, args, options) {
            try {
                return threadFn(workloads, args, options);
            } finally {
                if (typeof db !== 'undefined') {
                    db = null;
                    gc();
                }
            }
        };

        if (executionMode.composed) {
            return new ScopedThread(
                guardedThreadFn, workerThread.composed, workloads, args, options);
        }

        return new ScopedThread(guardedThreadFn, workerThread.fsm, workloads, args, options);
    }

    var latch;
    var errorLatch;
    var numThreads;

    var initialized = false;
    var threads = [];

    var _workloads, _context;

    this.init = function init(workloads, context, maxAllowedThreads) {
        assert.eq(
            'number', typeof maxAllowedThreads, 'the maximum allowed threads must be a number');
        assert.gt(maxAllowedThreads, 0, 'the maximum allowed threads must be positive');
        assert.eq(maxAllowedThreads,
                  Math.floor(maxAllowedThreads),
                  'the maximum allowed threads must be an integer');

        function computeNumThreads() {
            // If we don't have any workloads, such as having no background workloads, return 0.
            if (workloads.length === 0) {
                return 0;
            }
            return Array.sum(workloads.map(function(workload) {
                return context[workload].config.threadCount;
            }));
        }

        var requestedNumThreads = computeNumThreads();
        if (requestedNumThreads > maxAllowedThreads) {
            // Scale down the requested '$config.threadCount' values to make
            // them sum to less than 'maxAllowedThreads'
            var factor = maxAllowedThreads / requestedNumThreads;
            workloads.forEach(function(workload) {
                var config = context[workload].config;
                var threadCount = config.threadCount;
                threadCount = Math.floor(factor * threadCount);
                threadCount = Math.max(1, threadCount);  // ensure workload is executed
                config.threadCount = threadCount;
            });
        }

        numThreads = computeNumThreads();
        assert.lte(numThreads, maxAllowedThreads);
        latch = new CountDownLatch(numThreads);
        errorLatch = new CountDownLatch(numThreads);

        var plural = numThreads === 1 ? '' : 's';
        print('Using ' + numThreads + ' thread' + plural + ' (requested ' + requestedNumThreads +
              ')');

        _workloads = workloads;
        _context = context;

        initialized = true;
    };

    this.spawnAll = function spawnAll(cluster, options) {
        if (!initialized) {
            throw new Error('thread manager has not been initialized yet');
        }

        var workloadData = {};
        var tid = 0;
        _workloads.forEach(function(workload) {
            var config = _context[workload].config;
            workloadData[workload] = config.data;
            var workloads = [workload];  // worker thread only needs to load 'workload'
            if (executionMode.composed) {
                workloads = _workloads;  // worker thread needs to load all workloads
            }

            for (var i = 0; i < config.threadCount; ++i) {
                var args = {
                    tid: tid++,
                    data: workloadData,
                    host: cluster.getHost(),
                    latch: latch,
                    dbName: _context[workload].dbName,
                    collName: _context[workload].collName,
                    cluster: cluster.getSerializedCluster(),
                    clusterOptions: clusterOptions,
                    seed: Random.randInt(1e13),  // contains range of Date.getTime()
                    globalAssertLevel: globalAssertLevel,
                    errorLatch: errorLatch
                };

                var t = makeThread(workloads, args, options);
                threads.push(t);
                t.start();
            }
        });
    };

    this.checkFailed = function checkFailed(allowedFailurePercent) {
        if (!initialized) {
            throw new Error('thread manager has not been initialized yet');
        }

        var failedThreadIndexes = [];
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

        var failedThreads = failedThreadIndexes.length;
        if (failedThreads > 0) {
            print(failedThreads + ' thread(s) threw a JS or C++ exception while spawning');
        }

        if (failedThreads / threads.length > allowedFailurePercent) {
            throw new Error('Too many worker threads failed to spawn - aborting');
        }
    };

    this.checkForErrors = function checkForErrors() {
        if (!initialized) {
            throw new Error('thread manager has not been initialized yet');
        }

        // Each worker thread receives the errorLatch as an argument. The worker thread
        // decreases the count when it receives an error.
        return errorLatch.getCount() < numThreads;
    };

    this.joinAll = function joinAll() {
        if (!initialized) {
            throw new Error('thread manager has not been initialized yet');
        }

        var errors = [];

        threads.forEach(function(t) {
            t.join();

            var data = t.returnData();
            if (data && !data.ok) {
                errors.push(data);
            }
        });

        initialized = false;
        threads = [];

        return errors;
    };

    this.markAllForTermination = function markAllForTermination() {
        if (_workloads.length === 0) {
            return;
        }

        // Background threads periodically check the 'fsm_background' collection of the
        // 'config' database for a document specifying { terminate: true }. If such a
        // document is found the background thread terminates.
        var coll = _context[_workloads[0]].db.getSiblingDB('config').fsm_background;
        assert.writeOK(coll.update({terminate: true}, {terminate: true}, {upsert: true}));

    };
};

/**
 * Extensions to the 'workerThread' module for executing a single FSM-based
 * workload and a composition of them, respectively.
 */

workerThread.fsm = function(workloads, args, options) {
    load('jstests/concurrency/fsm_libs/worker_thread.js');  // for workerThread.main
    load('jstests/concurrency/fsm_libs/fsm.js');            // for fsm.run

    return workerThread.main(workloads, args, function(configs) {
        var workloads = Object.keys(configs);
        assert.eq(1, workloads.length);
        fsm.run(configs[workloads[0]]);
    });
};

workerThread.composed = function(workloads, args, options) {
    load('jstests/concurrency/fsm_libs/worker_thread.js');  // for workerThread.main
    load('jstests/concurrency/fsm_libs/composer.js');       // for composer.run

    return workerThread.main(workloads, args, function(configs) {
        composer.run(workloads, configs, options);
    });
};
