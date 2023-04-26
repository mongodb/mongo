/**
 * Tests crash recovery with the history store. Runs a workload while repeatedly killing all the
 * nodes of the replica set. Finally ensures that the db hashes match.
 *
 * @tags: [multiversion_incompatible, requires_persistence, requires_replication]
 */
(function() {
"use strict";

load("jstests/libs/parallel_shell_helpers.js");  // For funWithArgs

function workload(iteration) {
    load("jstests/libs/parallelTester.js");  // For Thread().

    const nthreads = 50;
    let threads = [];

    // The workload consists of 50 threads, each of which continuously inserts a document and bloats
    // the inserted document.
    for (let t = 0; t < nthreads; t++) {
        const thread = new Thread(function(t, iteration) {
            for (let i = 0;; i++) {
                const id =
                    db.c.insertMany([{iteration: iteration, doc: i, t: t, count: 0, entries: []}])
                        .insertedIds[0];
                for (let j = 0; j < 200; j++) {
                    const entry = {bigField: new Array(120).fill(j)};
                    db.c.update({_id: id}, {$inc: {count: 1}, $push: {entries: entry}});
                }
            }
        }, t, iteration);
        threads.push(thread);

        thread.start();
    }

    // We expect each thread to throw, because at some point during its execution, all the nodes
    // will be killed.
    for (var t = 0; t < nthreads; t++) {
        assert.throws(() => {
            threads[t].join();
        });
    }
}

const rst = new ReplSetTest({
    name: jsTestName() + "_rst",
    // Only one node is allowed to step up and become primary. This is to prevent synchronization
    // issues where a former primary but now secondary may enter rollback on finding that its
    // oplog has diverged.
    nodes: [{}, {rsConfig: {priority: 0}}, {rsConfig: {priority: 0}}],
    // Set the wiredTigerCacheSizeGB to stress the WT cache and create conditions for the usage of
    // the history store.
    // Set slowms to avoid logging "Slow query" lines. We expect many of them, and so we don't want
    // to clog the log file.
    nodeOptions: {wiredTigerCacheSizeGB: 0.25, slowms: 30000}
});

rst.startSet();
rst.initiate();

for (let i = 0; i < 10; i++) {
    jsTestLog("About to start workload, iteration: " + i);
    const workloadThread = startParallelShell(funWithArgs(workload, i),
                                              rst.getPrimary().port,
                                              false, /* noConnect */
                                              '--quiet' /* Make the shell less verbose */);

    // Allow the workload to run for several seconds as we sleep.
    const sleepSeconds = Math.floor(15 + Math.random() * 45);
    jsTestLog("Sleeping for " + sleepSeconds + " seconds, before killing nodes.");
    sleep(sleepSeconds * 1000);

    jsTestLog("Killing all nodes.");
    for (let i = 0; i < 3; i++) {
        const pid = rst.getNodeId(rst.nodes[i]);
        try {
            rst.stop(
                pid, 9 /* signal */, {skipValidation: true}, {forRestart: true, waitpid: true});
            assert(false, "Expected error after killing node.");
        } catch (e) {
            assert.eq(e.returnCode, MongoRunner.EXIT_SIGKILL, e);
            jsTestLog("Node killed as expected: " + e);
        }
    }

    workloadThread();

    jsTestLog("Restarting repl set.");
    const result = rst.startSet({wiredTigerCacheSizeGB: 0.25}, true /* restart */);
    assert.eq(result.length, 3, result);

    // Wait until the primary and secondaries are up and running.
    jsTestLog("Waiting for primary and secondaries.");
    rst.getPrimary();
    rst.awaitSecondaryNodes();

    jsTestLog("Checking that the db hashes match.");
    rst.checkReplicatedDataHashes();
}

rst.stopSet();
})();
