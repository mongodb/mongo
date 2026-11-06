/**
 * Verifies that disableReplicationNetworkCompression prevents replication data connections
 * (OplogFetcher, InitialSyncCloner, Rollback) from negotiating network compression, while other
 * connections are unaffected. Evidence is the sync source's "client metadata" (51800) log line,
 * which records the compressors negotiated on each incoming connection.
 *
 * @tags: [requires_replication]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {RollbackTest} from "jstests/replsets/libs/rollback_test.js";

const kCompressor = "zstd";
// Keep enough RAM log lines that earlier handshakes are not evicted during the test.
const kRamLogMaxLines = 20000;

function nodeOptions(disable) {
    return {
        networkMessageCompressors: kCompressor,
        setParameter: {disableReplicationNetworkCompression: disable, ramLogMaxLines: kRamLogMaxLines},
    };
}

// Returns the 51800 entries on 'conn' whose application name is 'appName'. Returns null when the
// log could not be read (e.g. a transient network error) so callers inside assert.soon retry.
function getClientMetadataEntries(conn, appName) {
    const lines = checkLog.getGlobalLog(conn);
    if (lines === null) {
        return null;
    }
    const result = [];
    for (const line of lines) {
        if (!line.includes('"id":51800')) {
            continue;
        }
        let entry;
        try {
            entry = JSON.parse(line);
        } catch (e) {
            continue;
        }
        if (entry.id !== 51800 || !entry.attr) {
            continue;
        }
        const doc = entry.attr.doc;
        if (doc && doc.application && doc.application.name === appName) {
            assert(Array.isArray(entry.attr.negotiatedCompressors), {entry});
            result.push({remote: entry.attr.remote, compressors: entry.attr.negotiatedCompressors});
        }
    }
    return result;
}

// Waits until 'conn' has logged more than 'baseline' connections for 'appName' and returns the
// entries logged after the baseline.
function waitForNewEntries(conn, appName, baseline) {
    let entries;
    assert.soon(
        () => {
            entries = getClientMetadataEntries(conn, appName);
            return entries !== null && entries.length > baseline;
        },
        () => `no new '${appName}' client metadata log line on ${conn.host}`,
    );
    return entries.slice(baseline);
}

function countEntries(conn, appName) {
    let entries;
    assert.soon(() => {
        entries = getClientMetadataEntries(conn, appName);
        return entries !== null;
    });
    return entries.length;
}

function assertNegotiated(entries, appName, disable) {
    for (const {compressors} of entries) {
        if (disable) {
            assert.eq([], compressors, `${appName} negotiated compression`, {entries});
        } else {
            assert(compressors.includes(kCompressor), `${appName} did not negotiate compression`, {
                entries,
            });
        }
    }
}

function assertParameter(conn, expected) {
    assert.eq(
        expected,
        assert.commandWorked(conn.adminCommand({getParameter: 1, disableReplicationNetworkCompression: 1}))
            .disableReplicationNetworkCompression,
    );
}

function testSteadyStateAndInitialSync(disable) {
    jsTest.log.info(`Steady state and initial sync, disableReplicationNetworkCompression=${disable}`);
    const rst = new ReplSetTest({nodes: [{}, {rsConfig: {priority: 0}}], nodeOptions: nodeOptions(disable)});
    rst.startSet();
    try {
        rst.initiate();
        const primary = rst.getPrimary();
        rst.nodes.forEach((node) => assertParameter(node, disable));

        // The parameter is startup-only.
        assert.commandFailedWithCode(
            primary.adminCommand({setParameter: 1, disableReplicationNetworkCompression: !disable}),
            ErrorCodes.IllegalOperation,
        );
        assertParameter(primary, disable);

        // Steady state: OplogFetcher connections from the secondary to the primary.
        const coll = primary.getDB("test").coll;
        const docs = [];
        for (let i = 0; i < 100; i++) {
            docs.push({_id: i, payload: "x".repeat(1000)});
        }
        assert.commandWorked(coll.insert(docs));
        rst.awaitReplication();
        assertNegotiated(waitForNewEntries(primary, "OplogFetcher", 0), "OplogFetcher", disable);

        // Initial sync of a new node. Pin its sync source to the primary and only inspect
        // connections logged after the node was added.
        const clonerBaseline = countEntries(primary, "InitialSyncCloner");
        const fetcherBaseline = countEntries(primary, "OplogFetcher");
        const newNode = rst.add(
            Object.merge(nodeOptions(disable), {
                rsConfig: {priority: 0},
                setParameter: Object.merge(nodeOptions(disable).setParameter, {
                    initialSyncSourceReadPreference: "primary",
                }),
            }),
        );
        rst.reInitiate();
        rst.awaitSecondaryNodes();
        rst.awaitReplication();
        assert.eq(100, newNode.getDB("test").coll.find().itcount());

        assertNegotiated(
            waitForNewEntries(primary, "InitialSyncCloner", clonerBaseline),
            "InitialSyncCloner",
            disable,
        );
        assertNegotiated(waitForNewEntries(primary, "OplogFetcher", fetcherBaseline), "OplogFetcher", disable);

        // Non-replication connections still negotiate compression.
        const shellApp = "disable_repl_compression_shell";
        const exitCode = runMongoProgram(
            "mongo",
            `mongodb://${primary.host}/?appName=${shellApp}`,
            `--networkMessageCompressors=${kCompressor}`,
            "--eval",
            "assert.commandWorked(db.adminCommand({hello: 1}));",
        );
        assert.eq(0, exitCode);
        const shellEntries = waitForNewEntries(primary, shellApp, 0);
        assertNegotiated(shellEntries, shellApp, false /* disable */);
    } finally {
        rst.stopSet();
    }
}

function testRollback(disable) {
    jsTest.log.info(`Rollback, disableReplicationNetworkCompression=${disable}`);
    const rollbackTest = new RollbackTest(jsTestName(), undefined, nodeOptions(disable));
    let stopped = false;
    try {
        rollbackTest.transitionToRollbackOperations();
        rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
        // The new primary is the sync source the rollback node will connect to.
        const syncSource = rollbackTest.getPrimary();
        const baseline = countEntries(syncSource, "Rollback");
        rollbackTest.transitionToSyncSourceOperationsDuringRollback();
        rollbackTest.transitionToSteadyStateOperations();

        assertNegotiated(waitForNewEntries(syncSource, "Rollback", baseline), "Rollback", disable);
        rollbackTest.stop();
        stopped = true;
    } finally {
        if (!stopped) {
            rollbackTest.getTestFixture().stopSet();
        }
    }
}

// RollbackTest sets global TestData flags, so run all plain replica set cases first.
for (const disable of [true, false]) {
    testSteadyStateAndInitialSync(disable);
}
for (const disable of [true, false]) {
    testRollback(disable);
}
