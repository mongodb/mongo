import {Cluster} from "jstests/concurrency/fsm_libs/cluster.js";
import {runner} from "jstests/concurrency/fsm_libs/runner.js";
import {ThreadManager} from "jstests/concurrency/fsm_libs/thread_mgr.js";
import {DiscoverTopology, Topology} from "jstests/libs/discover_topology.js";

const validateExecutionOptions = runner.internals.validateExecutionOptions;
const prepareCollections = runner.internals.prepareCollections;
const WorkloadFailure = runner.internals.WorkloadFailure;
const throwError = runner.internals.throwError;
const setupWorkload = runner.internals.setupWorkload;
const teardownWorkload = runner.internals.teardownWorkload;
const setIterations = runner.internals.setIterations;
const setThreadCount = runner.internals.setThreadCount;
const loadWorkloadContext = runner.internals.loadWorkloadContext;

// Returns true if the workload's teardown succeeds and false if the workload's teardown fails.
function cleanupWorkload(workload, context, cluster, errors, header) {
    try {
        teardownWorkload(workload, context, cluster);
    } catch (e) {
        errors.push(new WorkloadFailure(e.toString(), e.stack, 'main', header + ' Teardown'));
        return false;
    }

    return true;
}

// Writes to the specified FSM synchronization files, suffixed by the
// hook name for each hook.
function writeFiles(file) {
    for (const hook of TestData.useActionPermittedFile) {
        const path = file + '_' + hook;
        writeFile(path, '');
    }
}

// Attempts to 'cat' the acknowledgement file produced by each hook
// following the FSM synchronization protocol.
function readAcks(file) {
    for (const hook of TestData.useActionPermittedFile) {
        const path = file + '_' + hook;
        // The cat() function throws if the file isn't found.
        cat(path);
    }
}

async function runWorkloads(workloads,
                            {cluster: clusterOptions = {}, execution: executionOptions = {}} = {}) {
    assert.gt(workloads.length, 0, 'need at least one workload to run');

    const executionMode = {serial: true};
    validateExecutionOptions(executionMode, executionOptions);
    Object.freeze(executionOptions);  // immutable after validation (and normalization)

    const context = {};
    const applyMultipliers = true;
    await loadWorkloadContext(workloads, context, executionOptions, applyMultipliers);

    // Constructing a Cluster instance calls its internal validateClusterOptions() function,
    // which fills in any properties that aren't explicitly present in 'clusterOptions'. We do
    // this before constructing a ThreadManager instance to make its dependency on the
    // 'clusterOptions' being filled in explicit.
    const cluster = new Cluster(clusterOptions, executionOptions.sessionOptions);
    const threadMgr = new ThreadManager(clusterOptions);

    Random.setRandomSeed(clusterOptions.seed);

    const errors = [];
    const cleanup = [];
    let teardownFailed = false;
    let startTime = Date.now();  // Initialize in case setupWorkload fails below.
    let totalTime;

    await cluster.setup();

    if (typeof executionOptions.tenantId !== 'undefined') {
        // Import simulate_atlas_proxy.js to override requests for tenant during preparing
        // collections, setup and teardown of workloads.
        await import("jstests/libs/override_methods/simulate_atlas_proxy.js");
    }

    jsTest.log('Workload(s) started: ' + workloads.join(' '));

    prepareCollections(workloads, context, cluster, clusterOptions, executionOptions);

    try {
        // Set up the thread manager for this set of workloads.
        startTime = Date.now();

        {
            const maxAllowedThreads = 100 * executionOptions.threadMultiplier;
            threadMgr.init(workloads, context, maxAllowedThreads);
        }

        // Call each workload's setup function.
        workloads.forEach(function(workload) {
            // Define "iterations" and "threadCount" properties on the workload's $config.data
            // object so that they can be used within its setup(), teardown(), and state
            // functions. This must happen after calling threadMgr.init() in case the thread
            // counts needed to be scaled down.
            setIterations(context[workload].config);
            setThreadCount(context[workload].config);

            setupWorkload(workload, context, cluster);
            cleanup.push(workload);
        });

        // Await replication after running the $config.setup() function when actions are
        // permitted to ensure its effects aren't rolled back.
        if (cluster.isReplication() && executionOptions.actionFiles !== undefined) {
            cluster.awaitReplication();
        }

        // Synchronize the cluster times across all routers, so the earliest global snapshots chosen
        // by each router will include the effects of each setup function.
        if (cluster.isSharded()) {
            cluster.synchronizeMongosClusterTimes();
        }

        // After the $config.setup() function has been called, it is safe for the hook thread
        // to start running. The main thread won't attempt to interact with the cluster until
        // all of the spawned worker threads have finished.
        //

        // Indicate that the hook thread can run. It is unnecessary for the hook thread to
        // indicate that it is going to start running because it will eventually after the
        // worker threads have started.
        if (executionOptions.actionFiles !== undefined) {
            writeFiles(executionOptions.actionFiles.permitted);
        }

        // Since the worker threads may be running with causal consistency enabled, we set the
        // initial clusterTime and initial operationTime for the sessions they'll create so that
        // they are guaranteed to observe the effects of the workload's $config.setup() function
        // being called.
        if (typeof executionOptions.sessionOptions === 'object' &&
            executionOptions.sessionOptions !== null) {
            // We only start a session for the worker threads and never start one for the main
            // thread. We can therefore get the clusterTime and operationTime tracked by the
            // underlying DummyDriverSession through any DB instance (i.e. the "test" database
            // here was chosen arbitrarily).
            const session = cluster.getDB('test').getSession();

            // JavaScript objects backed by C++ objects (e.g. BSON values from a command
            // response) do not serialize correctly when passed through the Thread
            // constructor. To work around this behavior, we instead pass a stringified form of
            // the JavaScript object through the Thread constructor and use eval() to
            // rehydrate it.
            executionOptions.sessionOptions.initialClusterTime = tojson(session.getClusterTime());
            executionOptions.sessionOptions.initialOperationTime =
                tojson(session.getOperationTime());
        }

        try {
            try {
                // Start this set of worker threads.
                threadMgr.spawnAll(cluster, executionOptions);
                // Allow 20% of the threads to fail. This allows the workloads to run on
                // underpowered test hosts.
                threadMgr.checkFailed(0.2);
            } finally {
                // Threads must be joined before destruction, so do this even in the presence of
                // exceptions.
                errors.push(...threadMgr.joinAll().map(
                    e => new WorkloadFailure(
                        e.err, e.stack, e.tid, 'Foreground ' + e.workloads.join(' '))));
            }
        } finally {
            // Until we are guaranteed that the hook thread isn't running, it isn't safe for
            // the $config.teardown() function to be called. We should signal to resmoke.py that
            // the hook thread should stop running and wait for the hook thread to signal that
            // it has stopped.
            //
            // Signal to the hook thread to stop any actions.
            if (executionOptions.actionFiles !== undefined) {
                writeFiles(executionOptions.actionFiles.idleRequest);

                // Wait for the acknowledgement file to be created by the hook thread.
                assert.soonNoExcept(function() {
                    try {
                        // The readAcks() function will throw an exception if any hook hasn't
                        // provided an acknowledgement.
                        readAcks(executionOptions.actionFiles.idleAck);
                    } catch (ex) {
                        if (ex.code == 13300 /* CANT_OPEN_FILE */) {
                            // capture this exception to prevent soonNoExcept polluting the
                            // logs with errors
                            return false;
                        }
                        throw ex;
                    }
                    return true;
                }, "thread action still in progress");
            }
        }
    } finally {
        if (cluster.shouldPerformContinuousStepdowns()) {
            cluster.reestablishConnectionsAfterFailover();
        }
        // Call each workload's teardown function. After all teardowns have completed check if
        // any of them failed.
        const cleanupResults = cleanup.map(
            workload => cleanupWorkload(workload, context, cluster, errors, 'Foreground'));
        teardownFailed = cleanupResults.some(success => (success === false));

        totalTime = Date.now() - startTime;
        jsTest.log('Workload(s) completed in ' + totalTime + ' ms: ' + workloads.join(' '));
    }

    // Throw any existing errors so that resmoke.py can abort its execution of the test suite.
    throwError(errors);

    cluster.teardown();
}

if (typeof db === 'undefined') {
    throw new Error(
        'resmoke_runner.js must be run with the mongo shell already connected to the database');
}

const clusterOptions = {
    replication: {enabled: false},
    sharded: {enabled: false},
};

// The TestData.discoverTopoloy is false when we only care about connecting to either a
// standalone or primary node in a replica set.
if (TestData.discoverTopology !== false) {
    const topology = DiscoverTopology.findConnectedNodes(db.getMongo());

    if (topology.type === Topology.kReplicaSet) {
        clusterOptions.replication.enabled = true;
        clusterOptions.replication.numNodes = topology.nodes.length;
    } else if (topology.type === Topology.kShardedCluster) {
        clusterOptions.replication.enabled = true;
        clusterOptions.sharded.enabled = true;
        clusterOptions.sharded.enableBalancer =
            TestData.hasOwnProperty('runningWithBalancer') ? TestData.runningWithBalancer : true;
        clusterOptions.sharded.numMongos = topology.mongos.nodes.length;
        clusterOptions.sharded.numShards = Object.keys(topology.shards).length;
        clusterOptions.sharded.stepdownOptions = {};
        clusterOptions.sharded.stepdownOptions.configStepdown =
            TestData.runningWithConfigStepdowns || false;
        clusterOptions.sharded.stepdownOptions.shardStepdown =
            TestData.runningWithShardStepdowns || false;
    } else if (topology.type !== Topology.kStandalone) {
        throw new Error('Unrecognized topology format: ' + tojson(topology));
    }
}

clusterOptions.sameDB = TestData.sameDB;
clusterOptions.sameCollection = TestData.sameCollection;

let workloads = TestData.fsmWorkloads;

let sessionOptions = {};
if (TestData.runningWithCausalConsistency !== undefined) {
    // Explicit sessions are causally consistent by default, so causal consistency has to be
    // explicitly disabled.
    sessionOptions.causalConsistency = TestData.runningWithCausalConsistency;

    if (TestData.runningWithCausalConsistency) {
        sessionOptions.readPreference = {mode: 'secondary'};
    }
}

if (TestData.runningWithConfigStepdowns || TestData.runningWithShardStepdowns) {
    sessionOptions.retryWrites = true;
}

const executionOptions = {
    dbNamePrefix: TestData.dbNamePrefix || "",
    tenantId: TestData.tenantId
};
const resmokeDbPathPrefix = TestData.resmokeDbPathPrefix || ".";

// The action file names need to match the same construction as found in
// buildscripts/resmokelib/testing/hooks/lifecycle.py.
if (TestData.useActionPermittedFile) {
    assert(
        Array.isArray(TestData.useActionPermittedFile),
        `TestData.useActionPermittedFile needs to be a list of hooks use action files. Current value: '${
            tojson()}'`);
    executionOptions.actionFiles = {
        permitted: resmokeDbPathPrefix + '/permitted',
        idleRequest: resmokeDbPathPrefix + '/idle_request',
        idleAck: resmokeDbPathPrefix + '/idle_ack',
    };
}

if (Object.keys(sessionOptions).length > 0 || TestData.runningWithSessions) {
    executionOptions.sessionOptions = sessionOptions;
}

await runWorkloads(workloads, {cluster: clusterOptions, execution: executionOptions});
