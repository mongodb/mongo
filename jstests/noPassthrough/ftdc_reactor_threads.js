/**
 * Verify the FTDC metrics for reactor threads.
 *
 * @tags: [
 *   requires_sharding,
 * ]
 */
load('jstests/libs/fail_point_util.js');
load('jstests/libs/ftdc.js');
load('jstests/libs/parallelTester.js');

(function() {
'use strict';

function getDiagnosticData(mongos) {
    let db = mongos.getDB('admin');
    const stats = verifyGetDiagnosticData(db);
    assert(stats.hasOwnProperty('networkInterfaceStats'));
    return stats.networkInterfaceStats;
}

const numThreads = 50;
const ftdcPath = MongoRunner.toRealPath('ftdc');
const st = new ShardingTest({
    shards: 1,
    mongos: {
        s0: {setParameter: {diagnosticDataCollectionDirectoryPath: ftdcPath}},
    }
});

// Block operations after they acquire a connection.
const fp = 'networkInterfaceHangCommandsAfterAcquireConn';
let res = assert.commandWorked(st.s.adminCommand({configureFailPoint: fp, mode: 'alwaysOn'}));

// Run some operations and have them all blocked after acquiring a connection.
let threads = [];
for (var t = 0; t < numThreads; t++) {
    let thread = new Thread(function(connStr, t) {
        var conn = new Mongo(connStr);
        conn.getDB('test').runCommand({insert: 'test', documents: [{counter: t}]});
    }, st.s.host, t);
    threads.push(thread);
    thread.start();
}

// Wait for the reactor thread to block on the fail point.
// There is only one reactor thread for each network interface, and we can block that thread on `fp`
// by establishing new egress connections. The threads defined earlier trigger establishment of
// egress connections. Once the reactor thread is blocked, the number of times the server enters
// `fp` will no longer advance.
for (var t = 1; t <= numThreads; t++) {
    assert.neq(t, numThreads, "Failed to make the reactor thread block on the fail point");
    try {
        jsTestLog(`Checking fail point for times entered: ${res.count + t} ...`);
        assert.commandWorked(st.s.adminCommand({
            waitForFailPoint: fp,
            timesEntered: res.count + t,
            maxTimeMS: 10000,  // A large timeout to mitigate scheduling issues on slow machines.
        }));
    } catch (ex) {
        assert.commandFailedWithCode(ex, ErrorCodes.MaxTimeMSExpired);
        jsTestLog('The reactor thread should be blocked now!');
        break;
    }
}

// Introduce some delay before disabling the fail point and unblocking the operation.
sleep(5000);
assert.commandWorked(st.s.adminCommand({configureFailPoint: fp, mode: 'off'}));

for (var t = 0; t < numThreads; t++) {
    threads[t].join();
}

jsTestLog("Verifying FTDC metrics ...");
assert.soon(() => {
    const metrics = getDiagnosticData(st.s);

    // We expect at least one of the tasks scheduled on reactor threads to have a long run time.
    let longRunningTasks = 0;  // Tasks with a run time > 1 sec.
    for (const instance in metrics) {
        if (!metrics[instance].hasOwnProperty('runTime'))
            continue;  // Filter out FTDC metadata.
        longRunningTasks += metrics[instance]['runTime']['1000ms+'];
    }
    return longRunningTasks >= 1;
}, 'Expected to find at least one long running task', 30000);

st.stop();
})();
