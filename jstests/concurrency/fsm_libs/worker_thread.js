import {Cluster} from "jstests/concurrency/fsm_libs/cluster.js";
import {parseConfig} from "jstests/concurrency/fsm_libs/parse_config.js";
import {SpecificSecondaryReaderMongo} from "jstests/libs/specific_secondary_reader_mongo.js";

export const workerThread = (function() {
    // workloads = list of workload filenames
    // args.tid = the thread identifier
    // args.tenantId = the tenant id
    // args.data = map of workload -> 'this' parameter passed to the FSM state functions
    // args.host = the address to make a new connection to
    // args.latch = CountDownLatch instance for starting all threads
    // args.dbName = the database name
    // args.collName = the collection name
    // args.cluster = connection strings for all cluster nodes (see cluster.js for format)
    // args.clusterOptions = the configuration of the cluster
    // args.seed = seed for the random number generator
    // args.errorLatch = CountDownLatch instance that threads count down when they error
    // args.sessionOptions = the options to start a session with
    // run = callback that takes a map of workloads to their associated $config
    async function main(workloads, args, run) {
        var myDB;
        var configs = {};
        var connectionString = 'mongodb://' + args.host + '/?appName=tid:' + args.tid;
        if (typeof args.replSetName !== 'undefined') {
            connectionString += '&replicaSet=' + args.replSetName;
        }

        // The global 'TestData' object may still be undefined if the concurrency suite isn't being
        // run by resmoke.py (e.g. if it is being run via a parallel shell in the backup/restore
        // tests).
        TestData = (TestData !== undefined) ? TestData : {};

        try {
            // Running a callback passed through testData before running fsm worker theads.
            // Can be added to yml files as the following example:
            // fsmPreOverridesLoadedCallback: '
            //     testingReplication = true;
            //     await import("jstests/libs/override_methods/network_error_and_txn_override.js");
            //     ...
            // '
            if (typeof TestData.fsmPreOverridesLoadedCallback !== 'undefined') {
                new Function(`${TestData.fsmPreOverridesLoadedCallback}`)();
            }

            if (typeof db !== 'undefined') {
                // The implicit database connection created within the thread's scope
                // is unneeded, so forcibly clean it up.
                globalThis.db = undefined;
                gc();
            }

            let mongo;
            if (TestData.pinningSecondary) {
                mongo = new SpecificSecondaryReaderMongo(connectionString, args.secondaryHost);
            } else {
                mongo = new Mongo(connectionString);
            }

            if (typeof args.tenantId !== 'undefined') {
                TestData.tenantId = args.tenantId;
                await import("jstests/libs/override_methods/simulate_atlas_proxy.js");
            }

            // Retry operations that fail due to in-progress background operations. Load this early
            // so that later overrides can be retried.
            await import(
                "jstests/libs/override_methods/implicitly_retry_on_background_op_in_progress.js");

            if (typeof args.sessionOptions !== 'undefined') {
                let initialClusterTime;
                let initialOperationTime;

                // JavaScript objects backed by C++ objects (e.g. BSON values from a command
                // response) do not serialize correctly when passed through the Thread
                // constructor. To work around this behavior, we instead pass a stringified form
                // of the JavaScript object through the Thread constructor and use eval()
                // to rehydrate it.
                if (typeof args.sessionOptions.initialClusterTime === 'string') {
                    initialClusterTime = eval('(' + args.sessionOptions.initialClusterTime + ')');

                    // The initialClusterTime property was removed from SessionOptions in a
                    // later revision of the Driver's specification, so we remove the property
                    // and call advanceClusterTime() ourselves.
                    delete args.sessionOptions.initialClusterTime;
                }

                if (typeof args.sessionOptions.initialOperationTime === 'string') {
                    initialOperationTime =
                        eval('(' + args.sessionOptions.initialOperationTime + ')');

                    // The initialOperationTime property was removed from SessionOptions in a
                    // later revision of the Driver's specification, so we remove the property
                    // and call advanceOperationTime() ourselves.
                    delete args.sessionOptions.initialOperationTime;
                }

                const session = mongo.startSession(args.sessionOptions);
                const readPreference = session.getOptions().getReadPreference();
                if (readPreference && readPreference.mode === 'secondary') {
                    // Unset the explicit read preference so set_read_preference_secondary.js can do
                    // the right thing based on the DB.
                    session.getOptions().setReadPreference(undefined);

                    // We import set_read_preference_secondary.js in order to avoid running
                    // commands against the "admin" and "config" databases via mongos with
                    // readPreference={mode: "secondary"} when there's only a single node in
                    // the CSRS.
                    await import("jstests/libs/override_methods/set_read_preference_secondary.js");
                }

                if (typeof initialClusterTime !== 'undefined') {
                    session.advanceClusterTime(initialClusterTime);
                }

                if (typeof initialOperationTime !== 'undefined') {
                    session.advanceOperationTime(initialOperationTime);
                }

                myDB = session.getDatabase(args.dbName);
            } else {
                myDB = mongo.getDB(args.dbName);
            }

            {
                let connectionDesc = '';
                // In sharded environments, mongos is acting as a proxy for the mongo shell and
                // therefore has a different outbound port than the 'whatsmyuri' command returns.
                if (!Cluster.isSharded(args.clusterOptions) &&
                    !TestData.testingReplicaSetEndpoint) {
                    let res = assert.commandWorked(myDB.runCommand({whatsmyuri: 1}));
                    const myUri = res.you;

                    res = assert.commandWorked(myDB.adminCommand({currentOp: 1, client: myUri}));
                    connectionDesc = ', conn:' + res.inprog[0].desc;
                }

                const printOriginal = print;
                print = function() {
                    const printArgs = Array.from(arguments);
                    const prefix = '[tid:' + args.tid + connectionDesc + ']';
                    printArgs.unshift(prefix);
                    return printOriginal.apply(this, printArgs);
                };

                jsTestLog = function(msg) {
                    if (typeof msg === "object") {
                        msg = tojson(msg);
                    }
                    assert.eq(typeof (msg), "string", "Received: " + msg);
                    let msgs = msg.split("\n");
                    msgs = msgs.map(msg => '[tid:' + args.tid + '] ' + msg);
                    const printMsgs = ["----", ...msgs, "----"].map(s => `[jsTest] ${s}`);
                    printOriginal(`\n\n${printMsgs.join("\n")}\n\n`);
                };
            }

            if (Cluster.isReplication(args.clusterOptions)) {
                if (args.clusterOptions.hasOwnProperty('sharded') &&
                    args.clusterOptions.sharded.hasOwnProperty('stepdownOptions') &&
                    args.clusterOptions.sharded.stepdownOptions.shardStepdown) {
                    const newOptions = {
                        alwaysInjectTransactionNumber: true,
                        defaultReadConcernLevel: "majority",
                        logRetryAttempts: true,
                        overrideRetryAttempts: 3
                    };
                    Object.assign(TestData, newOptions);

                    assert(!TestData.hasOwnProperty('networkErrorAndTxnOverrideConfig'), TestData);
                    TestData.networkErrorAndTxnOverrideConfig = {retryOnNetworkErrors: true};
                    await import("jstests/libs/override_methods/network_error_and_txn_override.js");
                }

                // Operations that run after a "dropDatabase" command has been issued may fail with
                // a "DatabaseDropPending" error response if they would create a new collection on
                // that database while we're waiting for a majority of nodes in the replica set to
                // confirm it has been dropped. We load the
                // implicitly_retry_on_database_drop_pending.js file to make it so that the clients
                // started by the concurrency framework automatically retry their operation in the
                // face of this particular error response.
                await import(
                    "jstests/libs/override_methods/implicitly_retry_on_database_drop_pending.js");
            }

            if (TestData.defaultReadConcernLevel || TestData.defaultWriteConcern) {
                await import("jstests/libs/override_methods/set_read_and_write_concerns.js");
            }

            if (TestData.shardsAddedRemoved) {
                await import(
                    "jstests/libs/override_methods/implicitly_retry_on_shard_transition_errors.js");
            }

            for (const workload of workloads) {
                const {$config} = await import(workload);
                var config = parseConfig($config);  // to normalize

                // Copy any modifications that were made to $config.data
                // during the setup function of the workload (see caveat
                // below).

                // XXX: Changing the order of extend calls causes problems
                // for workloads that reference $super.
                // Suppose you have workloads A and B, where workload B extends
                // workload A. The $config.data of workload B can define a
                // function that closes over the $config object of workload A
                // (known as $super to workload B). This reference is lost when
                // the config object is serialized to BSON, which results in
                // undefined variables in the derived workload.
                var data = Object.extend({}, args.data[workload], true);
                data = Object.extend(data, config.data, true);

                // Object.extend() defines all properties added to the destination object as
                // configurable, enumerable, and writable. To prevent workloads from changing
                // the iterations and threadCount properties in their state functions, we redefine
                // them here as non-configurable and non-writable.
                Object.defineProperties(data, {
                    'iterations': {configurable: false, writable: false, value: data.iterations},
                    'threadCount': {configurable: false, writable: false, value: data.threadCount}
                });

                data.tid = args.tid;
                configs[workload] = {
                    data: data,
                    db: myDB,
                    collName: args.collName,
                    cluster: args.cluster,
                    iterations: data.iterations,
                    passConnectionCache: config.passConnectionCache,
                    startState: config.startState,
                    states: config.states,
                    tid: args.tid,
                    transitions: config.transitions,
                    errorLatch: args.errorLatch,
                    numThreads: args.numThreads,
                };
            }

            args.latch.countDown();

            // Converts any exceptions to a return status. In order for the
            // parent thread to call countDown() on our behalf, we must throw
            // an exception. Nothing prior to (and including) args.latch.countDown()
            // should be wrapped in a try/catch statement.
            try {
                args.latch.await();  // wait for all threads to start

                Random.setRandomSeed(args.seed);
                await run(configs);
                return {ok: 1};
            } catch (e) {
                jsTest.log('Thread failed. Other threads will stop execution on next iteration.');
                args.errorLatch.countDown();
                return {
                    ok: 0,
                    err: e.toString(),
                    stack: e.stack,
                    tid: args.tid,
                    workloads: workloads,
                };
            }
        } finally {
            // Kill this worker thread's session to ensure any possible idle cursors left open by
            // the workload are closed.
            // TODO SERVER-74993: Remove this.
            try {
                var session = myDB.getSession();
                if (session) {
                    myDB.runCommand({killSessions: [session.getSessionId()]});
                }
            } catch (e) {
                // Ignore errors from killSessions.
                jsTest.log('Error running killSessions: ' + e);
            }

            // Avoid retention of connection object
            configs = null;
            myDB = null;
            gc();
        }
    }

    return {main: main};
})();
