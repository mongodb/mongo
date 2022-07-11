/**
 * Runs dbCheck in background.
 */
'use strict';

(function() {
load('jstests/libs/discover_topology.js');  // For Topology and DiscoverTopology.
load('jstests/libs/parallelTester.js');     // For Thread.

if (typeof db === 'undefined') {
    throw new Error(
        "Expected mongo shell to be connected a server, but global 'db' object isn't defined");
}

TestData = TestData || {};

// Disable implicit sessions so FSM workloads that kill random sessions won't interrupt the
// operations in this test that aren't resilient to interruptions.
TestData.disableImplicitSessions = true;

const conn = db.getMongo();
const topology = DiscoverTopology.findConnectedNodes(conn);

const exceptionFilteredBackgroundDbCheck = function(hosts) {
    const runBackgroundDbCheck = function(hosts) {
        const quietly = (func) => {
            const printOriginal = print;
            try {
                print = Function.prototype;
                func();
            } finally {
                print = printOriginal;
            }
        };

        let rst;
        // We construct the ReplSetTest instance with the print() function overridden to be a no-op
        // in order to suppress the log messages about the replica set configuration. The
        // run_dbcheck_background.js hook is executed frequently by resmoke.py and would
        // otherwise lead to generating an overwhelming amount of log messages.
        quietly(() => {
            rst = new ReplSetTest(hosts[0]);
        });

        const dbNames = new Set();
        const primary = rst.getPrimary();

        const version = assert
                            .commandWorked(primary.adminCommand(
                                {getParameter: 1, featureCompatibilityVersion: 1}))
                            .featureCompatibilityVersion.version;
        if (version != latestFCV) {
            print("Not running dbCheck in FCV " + version);
            return {ok: 1};
        }

        print("Running dbCheck for: " + rst.getURL());

        const adminDb = primary.getDB('admin');
        let res = assert.commandWorked(adminDb.runCommand({listDatabases: 1, nameOnly: true}));
        for (let dbInfo of res.databases) {
            dbNames.add(dbInfo.name);
        }

        // Transactions cannot be run on the following databases so we don't attempt to read at a
        // clusterTime on them either. (The "local" database is also not replicated.)
        // The config.transactions collection is different between primaries and secondaries.
        dbNames.delete('config');
        dbNames.delete('local');

        dbNames.forEach((dbName) => {
            assert.commandWorked(primary.getDB(dbName).runCommand({dbCheck: 1}));
            jsTestLog("dbCheck done on database " + dbName);

            const dbCheckCompleted = (db) => {
                return db.currentOp({$all: true}).inprog.filter(x => x["desc"] === "dbCheck")[0] ===
                    undefined;
            };

            assert.soon(() => dbCheckCompleted(adminDb),
                        "timed out waiting for dbCheck to finish on database: " + dbName);
        });

        // Wait for all secondaries to finish applying all dbcheck batches.
        rst.awaitReplication();

        const nodes = [
            rst.getPrimary(),
            ...rst.getSecondaries().filter(conn => {
                return !conn.adminCommand({isMaster: 1}).arbiterOnly;
            })
        ];
        nodes.forEach((node) => {
            // Assert no errors (i.e., found inconsistencies). Allow warnings. Tolerate
            // SnapshotTooOld errors, as they can occur if the primary is slow enough processing a
            // batch that the secondary is unable to obtain the timestamp the primary used.
            const healthlog = node.getDB('local').system.healthlog;
            // Regex matching strings that start without "SnapshotTooOld"
            const regexStringWithoutSnapTooOld = /^((?!^SnapshotTooOld).)*$/;

            // healthlog is a capped collection, truncation during scan might cause cursor
            // invalidation. Truncated data is most likely from previous tests in the fixture, so we
            // should still be able to catch errors by retrying.
            assert.soon(() => {
                try {
                    let errs = healthlog.find(
                        {"severity": "error", "data.error": regexStringWithoutSnapTooOld});
                    if (errs.hasNext()) {
                        const err = "dbCheck found inconsistency on " + node.host;
                        jsTestLog(err + ". Errors: ");
                        for (let count = 0; errs.hasNext() && count < 20; count++) {
                            jsTestLog(tojson(errs.next()));
                        }
                        assert(false, err);
                    }
                    return true;
                } catch (e) {
                    if (e.code !== ErrorCodes.CappedPositionLost) {
                        throw e;
                    }
                    jsTestLog(`Retrying on CappedPositionLost error: ${tojson(e)}`);
                    return false;
                }
            }, "healthlog scan could not complete.", 60000);

            jsTestLog("Checked health log on " + node.host);
        });

        return {ok: 1};
    };

    const onDrop = function(e) {
        jsTestLog("Skipping dbCheck due to transient error: " + tojson(e));
        return {ok: 1};
    };

    return assert.dropExceptionsWithCode(() => {
        return runBackgroundDbCheck(hosts);
    }, [ErrorCodes.NamespaceNotFound, ErrorCodes.LockTimeout, ErrorCodes.Interrupted], onDrop);
};

if (topology.type === Topology.kReplicaSet) {
    let res = exceptionFilteredBackgroundDbCheck(topology.nodes);
    assert.commandWorked(res, () => 'dbCheck replication consistency check failed: ' + tojson(res));
} else if (topology.type === Topology.kShardedCluster) {
    const threads = [];
    try {
        if (topology.configsvr.type === Topology.kReplicaSet) {
            const thread = new Thread(exceptionFilteredBackgroundDbCheck, topology.configsvr.nodes);
            threads.push(thread);
            thread.start();
        }

        for (let shardName of Object.keys(topology.shards)) {
            const shard = topology.shards[shardName];
            if (shard.type === Topology.kReplicaSet) {
                const thread = new Thread(exceptionFilteredBackgroundDbCheck, shard.nodes);
                threads.push(thread);
                thread.start();
            } else {
                throw new Error('Unrecognized topology format: ' + tojson(topology));
            }
        }
    } finally {
        // Wait for each thread to finish. Throw an error if any thread fails.
        let exception;
        const returnData = threads.map(thread => {
            try {
                thread.join();
                return thread.returnData();
            } catch (e) {
                if (!exception) {
                    exception = e;
                }
            }
        });
        if (exception) {
            throw exception;
        }

        returnData.forEach(res => {
            assert.commandWorked(
                res, () => 'dbCheck replication consistency check failed: ' + tojson(res));
        });
    }
} else {
    throw new Error('Unsupported topology configuration: ' + tojson(topology));
}
})();
