/**
 * SERVER-130410: verify the replication.networkCompression.compressors option (and its
 * setParameter form replicationNetworkCompression) controls, per node, which compressor(s) the
 * oplog fetcher advertises to its sync source WITHOUT affecting the process-wide
 * net.compression.compressors setting used by every other connection on the node.
 *
 * The main steady-state functional signal we assert on is the sync source's
 *   serverStatus().network.compression.<name>.compressor.bytesIn
 * counter: it grows when the sync source compresses oplog batches for a fetcher connection
 * negotiated with <name>. For receiving-side paths (initial sync cloner, rollback common-point
 * reader, and external client requests), the test uses the receiver's decompressor.bytesIn counter.
 *
 * The parameter is STARTUP-ONLY: it can be provided via the command line, the YAML config, or an
 * initial --setParameter, but cannot be changed at runtime. This test also verifies that a
 * runtime setParameter is refused.
 *
 * @tags: [requires_replication, requires_persistence]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {restartServerReplication, stopServerReplication} from "jstests/libs/write_concern_util.js";
import {waitForState} from "jstests/replsets/rslib.js";

// The default/inherit scenario advertises the full snappy/zstd/zlib set on the process-wide port.
// Explicit replication-compressor scenarios disable net compression and configure the requested
// replication compressor on both nodes, so process-wide decompressor counters stay attributable to
// the replication data channel under test.
const kProcessCompressors = "snappy,zstd,zlib";

// Read the sync source's per-compressor COMPRESSOR bytesIn counters. Returns a plain object
// keyed by compressor name so the test math is easy to read.
//
// The oplog fetcher pulls batches FROM the sync source over an exhaust cursor, so
// the sync source COMPRESSES those oplog responses on the fetcher's connection. That
// compressor direction carries the bulk replicated data and is the reliable signal for which
// algorithm the replication channel negotiated. The reverse (sync source decompressor) only sees
// the fetcher's tiny exhaust-cursor requests, which do not move measurable bytes when process-wide
// net compression is disabled - so a decompressor-based check reports a false "uncompressed" even
// though replication is compressed. Measuring the compressor avoids that false negative.
function getCompressorBytesIn(node) {
    const ss = assert.commandWorked(node.adminCommand({serverStatus: 1}));
    const out = {};
    if (!ss.network || !ss.network.compression) {
        return out;
    }
    for (const name of Object.keys(ss.network.compression)) {
        const c = ss.network.compression[name].compressor;
        out[name] = c ? c.bytesIn : 0;
    }
    return out;
}

// Read a node's per-compressor DECOMPRESSOR bytesIn counters. Returns a plain object keyed by
// compressor name.
//
// Use this on the RECEIVING side of a data transfer, i.e. the node that DECOMPRESSES
// inbound compressed messages:
//   - the initial-sync collection cloner and the rollback common-point reader pull bulk data FROM
//     the sync source as query responses, so the SYNCING/ROLLING-BACK node decompresses it, and
//   - an external client that pushes a large compressed request makes the SERVER it connects to
//     decompress it.
// For the steady-state oplog fetcher the bulk data flows the other way (the sync source compresses
// the oplog it streams to the fetcher), so measure that direction with getCompressorBytesIn() on the
// sync source instead - the sync source's decompressor only ever sees the fetcher's tiny exhaust
// requests and would report a false "uncompressed".
function getDecompressorBytesIn(node) {
    const ss = assert.commandWorked(node.adminCommand({serverStatus: 1}));
    const out = {};
    if (!ss.network || !ss.network.compression) {
        return out;
    }
    for (const name of Object.keys(ss.network.compression)) {
        const d = ss.network.compression[name].decompressor;
        out[name] = d ? d.bytesIn : 0;
    }
    return out;
}

// Read a node's serverStatus().repl.compression per-compressor byte counters, which
// account for the replication-data-plane subset of network.compression. `direction` is "compressor"
// or "decompressor". Returns {name: bytesIn}. Unlike the process-wide network.compression counters,
// these only include traffic on replication-marked connections, so they are attributable to the
// replication channel even when net compression is also enabled.
function getReplCompressionBytesIn(node, direction) {
    const ss = assert.commandWorked(node.adminCommand({serverStatus: 1}));
    const out = {};
    if (!ss.repl || !ss.repl.compression) {
        return out;
    }
    for (const name of Object.keys(ss.repl.compression)) {
        const d = ss.repl.compression[name][direction];
        out[name] = d ? d.bytesIn : 0;
    }
    return out;
}

// Diff two compressor snapshots; returns {name: delta} only for names that grew.
function decompressorDelta(before, after) {
    const grew = {};
    for (const name of Object.keys(after)) {
        const b = before[name] || 0;
        const a = after[name] || 0;
        if (a > b) {
            grew[name] = a - b;
        }
    }
    return grew;
}

const kReplicationCompressionNegotiationLogId = 10130422;

function hasReplicationCompressionNegotiationLog(node, expectedCompressed, expectedCompressors) {
    const log = assert.commandWorked(node.adminCommand({getLog: "global"})).log;
    return log.some((line) => {
        let entry;
        try {
            entry = JSON.parse(line);
        } catch (e) {
            return false;
        }
        if (entry.id !== kReplicationCompressionNegotiationLogId || !entry.attr) {
            return false;
        }
        if (entry.attr.compressed !== expectedCompressed) {
            return false;
        }
        if (expectedCompressors === undefined) {
            return true;
        }
        return bsonWoCompare(entry.attr.negotiatedCompressors || [], expectedCompressors) === 0;
    });
}

// Drive enough oplog traffic from the primary that any compressed replication channel is
// guaranteed to move measurable bytes through the sync source's compressor. Individual docs are
// made ~1 KiB and repeated so total payload comfortably exceeds any per-batch overhead, including
// on debug builds.
function generateOplogTraffic(primary, dbName, collName, iteration) {
    const coll = primary.getDB(dbName)[collName];
    const bulk = coll.initializeUnorderedBulkOp();
    const filler = "x".repeat(1024);
    for (let i = 0; i < 200; ++i) {
        bulk.insert({_id: `${iteration}-${i}`, payload: filler});
    }
    assert.commandWorked(bulk.execute({w: 2}));
}

// Run one scenario. `secondaryValue` is what we pass to the secondary as
// replicationNetworkCompression at startup (undefined = don't set it, i.e. inherit process
// default). `expectCompressorGrew` is the compressor name we expect to see compressor bytes on at
// the sync source, or null when we expect NO compressor bytes (uncompressed channel).
function runScenario({label, secondaryValue, expectCompressorGrew, extraForbid}) {
    jsTest.log.info(`[replicationNetworkCompression] scenario: ${label}`);

    // serverStatus().network.compression.* counters are process-wide, not connection-scoped. For
    // explicit replication-compression scenarios, disable the process-wide net compressor list on
    // both nodes and configure the replication compressor on both sides; otherwise heartbeat/internal
    // traffic could grow unrelated decompressor counters and make a correct replication channel look
    // like it used the wrong compressor.
    const explicitReplicationCompressor =
        secondaryValue !== undefined && secondaryValue !== "disabled";
    const useDisabledProcessCompressors =
        expectCompressorGrew === null || explicitReplicationCompressor;
    const processCompressors = useDisabledProcessCompressors ? "disabled" : kProcessCompressors;

    const primaryOpts = {
        networkMessageCompressors: processCompressors,
    };
    if (explicitReplicationCompressor) {
        primaryOpts.setParameter = {replicationNetworkCompression: secondaryValue};
    }

    const secondaryOpts = {
        networkMessageCompressors: processCompressors,
    };
    if (secondaryValue !== undefined) {
        secondaryOpts.setParameter = {replicationNetworkCompression: secondaryValue};
    }

    const rst = new ReplSetTest({
        name: `replication_network_compression_${label}`,
        nodes: [
            // Primary acts as sync source. Use the per-scenario process/replication compressor
            // settings so counter assertions cannot be polluted by unrelated net-compressed
            // internal traffic.
            primaryOpts,
            // Secondary is the node under test.
            secondaryOpts,
        ],
    });
    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();
    const secondary = rst.getSecondary();

    // Baseline: some traffic already flowed during initiate; snapshot process-wide and
    // repl.compression counters AFTER the secondary is up and steady so any growth we observe is
    // attributable to the writes we drive below on this fetcher's channel.
    rst.awaitReplication();
    const before = getCompressorBytesIn(primary);
    const replCompressorBefore = getReplCompressionBytesIn(primary, "compressor");
    const replDecompressorBefore = getReplCompressionBytesIn(secondary, "decompressor");

    // Drive several rounds of writes so the fetcher pulls new batches through its already-
    // established sync-source connection. We deliberately do NOT force a reconnect here: the
    // channel that came up during initiate is the one we want to observe.
    for (let i = 0; i < 5; ++i) {
        generateOplogTraffic(primary, "netcompress", "c", i);
    }
    rst.awaitReplication();

    const after = getCompressorBytesIn(primary);
    const grew = decompressorDelta(before, after);
    const replCompressorGrew = decompressorDelta(
        replCompressorBefore, getReplCompressionBytesIn(primary, "compressor"));
    const replDecompressorGrew = decompressorDelta(
        replDecompressorBefore, getReplCompressionBytesIn(secondary, "decompressor"));
    jsTest.log.info(
        `[replicationNetworkCompression] scenario ${label} compressor delta on sync source: ` +
            tojson(grew),
    );
    jsTest.log.info(
        `[replicationNetworkCompression] scenario ${label} repl.compression compressor delta: ` +
            tojson(replCompressorGrew),
    );
    jsTest.log.info(
        `[replicationNetworkCompression] scenario ${label} repl.compression decompressor delta: ` +
            tojson(replDecompressorGrew),
    );

    if (expectCompressorGrew === null) {
        // Disabled / empty-after-filter: the replication channel is uncompressed. This scenario
        // also disables process-wide net compression on both nodes, so no unrelated heartbeat or
        // internal RPC can grow the process-wide compressor counters.
        assert.eq(
            {},
            grew,
            `[${label}] expected no compressor growth on sync source (channel should be ` +
                `uncompressed), but saw: ${tojson(grew)}`,
        );
        assert.eq(
            {},
            replCompressorGrew,
            `[${label}] expected no repl.compression compressor growth on sync source, but saw: ` +
                tojson(replCompressorGrew),
        );
        assert.eq(
            {},
            replDecompressorGrew,
            `[${label}] expected no repl.compression decompressor growth on fetcher, but saw: ` +
                tojson(replDecompressorGrew),
        );
    } else {
        assert(
            grew[expectCompressorGrew] && grew[expectCompressorGrew] > 0,
            `[${label}] expected compressor '${expectCompressorGrew}' bytesIn to grow on ` +
                `sync source, delta was: ${tojson(grew)}`,
        );
        assert(
            replCompressorGrew[expectCompressorGrew] &&
                replCompressorGrew[expectCompressorGrew] > 0,
            `[${label}] expected repl.compression.${expectCompressorGrew}.compressor to grow on ` +
                `sync source, delta was: ${tojson(replCompressorGrew)}`,
        );
        assert(
            replDecompressorGrew[expectCompressorGrew] &&
                replDecompressorGrew[expectCompressorGrew] > 0,
            `[${label}] expected repl.compression.${expectCompressorGrew}.decompressor to grow ` +
                `on fetcher, delta was: ${tojson(replDecompressorGrew)}`,
        );
        // If the operator listed only one algorithm, no other compressor should have been
        // used on the replication channel.
        if (extraForbid) {
            for (const name of extraForbid) {
                assert(
                    !grew[name],
                    `[${label}] compressor '${name}' unexpectedly grew on sync source ` +
                        `despite not being in the allow-list: ${tojson(grew)}`,
                );
                assert(
                    !replCompressorGrew[name],
                    `[${label}] repl.compression compressor '${name}' unexpectedly grew on ` +
                        `sync source despite not being in the allow-list: ` +
                        tojson(replCompressorGrew),
                );
                assert(
                    !replDecompressorGrew[name],
                    `[${label}] repl.compression decompressor '${name}' unexpectedly grew on ` +
                        `fetcher despite not being in the allow-list: ` +
                        tojson(replDecompressorGrew),
                );
            }
        }
    }

    rst.stopSet();
}

// ---------------------------------------------------------------------------
// 1. Startup validation: bad values must be rejected up front, both via CLI/YAML and
//    setParameter. This locks in that a typo/malicious value can never silently degrade
//    replication.
// ---------------------------------------------------------------------------
(function testStartupRejectsInvalidValues() {
    jsTest.log.info("[replicationNetworkCompression] startup rejects invalid values");

    // "disabled" mixed with an algorithm name is illegal (parser must reject).
    assert.throws(() =>
        MongoRunner.runMongod({
            setParameter: {replicationNetworkCompression: "disabled,snappy"},
        }),
    );

    // Whitespace-only / garbage token is illegal.
    assert.throws(() =>
        MongoRunner.runMongod({
            setParameter: {replicationNetworkCompression: ","},
        }),
    );

    // A value that is only whitespace is non-empty but contains no compressor names, so it must be
    // rejected rather than silently treated as inherit (which is reserved for a truly empty "").
    for (const blank of [" ", "\t", "  ,  "]) {
        assert.throws(
            () =>
                MongoRunner.runMongod({
                    setParameter: {replicationNetworkCompression: blank},
                }),
            [],
            `replicationNetworkCompression=${tojson(blank)} (whitespace/separators only) must ` +
                `fail startup, not be treated as inherit`,
        );
    }
})();

// ---------------------------------------------------------------------------
// 2. Startup accepts and reflects every legal value in getParameter. This proves the
//    YAML/CLI -> setParameter seeding wiring in storeMongodOptions works.
// ---------------------------------------------------------------------------
(function testStartupAcceptsLegalValues() {
    jsTest.log.info("[replicationNetworkCompression] startup accepts legal values");
    const cases = ["", "disabled", "snappy", "zstd", "zstd,snappy"];
    for (const v of cases) {
        const conn = MongoRunner.runMongod({
            networkMessageCompressors: kProcessCompressors,
            setParameter: {replicationNetworkCompression: v},
        });
        assert.neq(null, conn, `mongod failed to start with replicationNetworkCompression="${v}"`);
        const res = assert.commandWorked(
            conn.adminCommand({getParameter: 1, replicationNetworkCompression: 1}),
        );
        assert.eq(
            v,
            res.replicationNetworkCompression,
            `getParameter did not echo back the value we set for "${v}": ${tojson(res)}`,
        );
        MongoRunner.stopMongod(conn);
    }
})();

// ---------------------------------------------------------------------------
// 3. Runtime setParameter must be REJECTED: replicationNetworkCompression is
//    declared set_at: [startup] only. Attempting to change it at runtime - with a legal value,
//    with an illegal value, and even with a value that equals the current one - must fail and
//    must NOT mutate the stored value. Changing compression requires editing the config /
//    command line and restarting mongod.
// ---------------------------------------------------------------------------
(function testRuntimeSetParameterRejected() {
    jsTest.log.info("[replicationNetworkCompression] runtime setParameter is rejected");
    const startupValue = "zstd";
    const conn = MongoRunner.runMongod({
        networkMessageCompressors: kProcessCompressors,
        setParameter: {replicationNetworkCompression: startupValue},
    });
    const admin = conn.getDB("admin");

    // Sanity: the startup value is present.
    let res = assert.commandWorked(
        admin.runCommand({getParameter: 1, replicationNetworkCompression: 1}),
    );
    assert.eq(startupValue, res.replicationNetworkCompression);

    // Every runtime setParameter attempt must fail, including one that would be a no-op:
    // startup-only enforcement must not depend on whether the value would actually change.
    for (const v of ["", "disabled", "snappy", "zstd,snappy", startupValue, "disabled,snappy"]) {
        const r = admin.runCommand({setParameter: 1, replicationNetworkCompression: v});
        assert.commandFailed(
            r,
            `runtime setParameter must be refused (startup-only) for value "${v}": ${tojson(r)}`,
        );
    }

    // Stored value must be exactly the startup value; no attempt above may have leaked through.
    res = assert.commandWorked(
        admin.runCommand({getParameter: 1, replicationNetworkCompression: 1}),
    );
    assert.eq(
        startupValue,
        res.replicationNetworkCompression,
        "runtime setParameter attempts must not mutate the stored startup value",
    );

    MongoRunner.stopMongod(conn);
})();

// ---------------------------------------------------------------------------
// 4. End-to-end functional scenarios on a 2-node replica set. Oplog fetching pulls batches FROM
//    the sync source, so the sync source COMPRESSES the responses and the fetcher decompresses
//    them. These scenarios therefore observe the sync source's compressor counters to prove the
//    fetcher's channel actually negotiated the expected algorithm (or no algorithm at all).
// ---------------------------------------------------------------------------

// Default (unset): inherit process-wide list -> some compression is negotiated. The exact
// algorithm depends on the server's negotiation policy given both sides list
// snappy,zstd,zlib; we just require that SOME compressor grew on the sync source, which
// distinguishes this case from the "disabled" case below.
runScenario({
    label: "default_inherits_process",
    secondaryValue: undefined,
    // We don't assert a specific name here; assert at least one grew via a custom check:
    // reuse the runner with a sentinel by picking the algorithm the process-wide policy will
    // pick. Both mongo negotiation and the manager keep the CLIENT's order, so the fetcher's
    // client-side order is the process-wide compressor list order = snappy first.
    expectCompressorGrew: "snappy",
});

// "disabled": the replication channel must be uncompressed; no compressor grows on the sync
// source as a result of fetcher traffic.
runScenario({
    label: "disabled",
    secondaryValue: "disabled",
    expectCompressorGrew: null,
});

// Single-algorithm subset: force zstd. This proves the allow-list actually narrows the
// negotiation: even though snappy is available process-wide (and would otherwise be chosen
// first by ordering), the fetcher only offered zstd, so the channel MUST end up using zstd
// and snappy/zlib decompressors must NOT have grown.
runScenario({
    label: "only_zstd",
    secondaryValue: "zstd",
    expectCompressorGrew: "zstd",
    extraForbid: ["snappy", "zlib"],
});

// Single-algorithm subset: force zlib. Same logic as above, different algorithm.
runScenario({
    label: "only_zlib",
    secondaryValue: "zlib",
    expectCompressorGrew: "zlib",
    extraForbid: ["snappy", "zstd"],
});

// Comma-separated preference order: "zstd,snappy". The client's advertised order is the
// negotiation preference; server picks the first entry it also supports, which is zstd.
runScenario({
    label: "zstd_then_snappy_prefers_zstd",
    secondaryValue: "zstd,snappy",
    expectCompressorGrew: "zstd",
    extraForbid: ["zlib"],
});

// ---------------------------------------------------------------------------
// 5. Isolation: replicationNetworkCompression must not affect client-facing traffic. This
//    test brings up a secondary with replicationNetworkCompression=disabled but the full
//    process-wide compressor list, then runs a shell client against it that negotiates
//    snappy. The client-facing decompressor counters on the SECONDARY must grow, proving that
//    disabling the replication channel did not accidentally disable client compression.
// ---------------------------------------------------------------------------
(function testDoesNotAffectClientFacingCompression() {
    jsTest.log.info("[replicationNetworkCompression] isolation: client-facing still compressed");
    const rst = new ReplSetTest({
        name: "replication_network_compression_isolation",
        nodes: [
            {networkMessageCompressors: kProcessCompressors},
            {
                networkMessageCompressors: kProcessCompressors,
                setParameter: {replicationNetworkCompression: "disabled"},
            },
        ],
    });
    rst.startSet();
    rst.initiate();
    const secondary = rst.getSecondary();

    // Open a NEW connection to the secondary that negotiates snappy on the client side. The
    // default Mongo shell connection used by rst.getSecondary() is uncompressed, so we build
    // a compressed one explicitly via the raw mongo program. Snapshot repl.compression too: external
    // client traffic must grow network.compression but must NOT be attributed to the replication
    // data-plane subset.
    const before = getDecompressorBytesIn(secondary);
    const replCompressorBefore = getReplCompressionBytesIn(secondary, "compressor");
    const replDecompressorBefore = getReplCompressionBytesIn(secondary, "decompressor");
    assert.eq(
        0,
        runMongoProgram(
            "mongo",
            "--host",
            secondary.host,
            "--networkMessageCompressors=snappy",
            "--eval",
            // Push enough bytes to trip the compressor's minimum-size gate.
            "for (let i = 0; i < 50; ++i) { assert.commandWorked(" +
                "db.adminCommand({hello: 1, payload: 'x'.repeat(4096)})); }",
        ),
        "compressed client shell against secondary failed",
    );
    const after = getDecompressorBytesIn(secondary);
    const grew = decompressorDelta(before, after);
    assert(
        grew["snappy"] && grew["snappy"] > 0,
        "client-facing snappy compression on the secondary should still work when " +
            "replicationNetworkCompression=disabled; delta was: " +
            tojson(grew),
    );

    // The same external client connection must not pollute serverStatus().repl.compression. This
    // directly verifies that the new repl.compression counters are a replication-marker subset, not
    // just another view of all network compression traffic.
    const replCompressorGrew = decompressorDelta(
        replCompressorBefore, getReplCompressionBytesIn(secondary, "compressor"));
    const replDecompressorGrew = decompressorDelta(
        replDecompressorBefore, getReplCompressionBytesIn(secondary, "decompressor"));
    assert.eq(
        {},
        replCompressorGrew,
        "external client response compression must not be counted in repl.compression compressor " +
            "counters; delta: " + tojson(replCompressorGrew),
    );
    assert.eq(
        {},
        replDecompressorGrew,
        "external client request compression must not be counted in repl.compression decompressor " +
            "counters; delta: " + tojson(replDecompressorGrew),
    );

    rst.stopSet();
})();

// ---------------------------------------------------------------------------
// 5b. Scoping: replicationNetworkCompression must scope to the replication
//     sync-source connection ONLY. Other internal (internalClient) connections - heartbeats and,
//     in a sharded cluster, intra-cluster RPC - must keep using net.compression.compressors. The
//     server distinguishes them via the "replicationCompressionClient" hello marker that only
//     replication data-plane clients send; heartbeats do not send it.
//
//     We assert this end-to-end with a node configured net=snappy, replication=disabled. Because
//     heartbeats between the two nodes are internal connections that do NOT carry the replication
//     marker, they must negotiate against the net set (snappy) rather than being forced
//     uncompressed by replication=disabled. We verify the cluster stays healthy while the net set is
//     otherwise active, and use the replication-client negotiation log to prove the marked data-plane
//     connection itself negotiated uncompressed even though net compression remained enabled.
// ---------------------------------------------------------------------------
(function testReplicationCompressionDoesNotDisableInternalNonReplicationConnections() {
    jsTest.log.info(
        "[replicationNetworkCompression] scoping: replication=disabled must not touch heartbeats");
    const rst = new ReplSetTest({
        name: "replication_network_compression_scoping",
        nodes: [
            {networkMessageCompressors: "snappy"},
            {
                networkMessageCompressors: "snappy",
                setParameter: {replicationNetworkCompression: "disabled"},
            },
        ],
    });
    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();
    const secondary = rst.getSecondary();

    assert.soon(
        () => hasReplicationCompressionNegotiationLog(secondary, false, []),
        "secondary did not log an uncompressed replication compression negotiation while " +
            "replicationNetworkCompression=disabled and net compression stayed enabled",
    );

    // Drive fresh replication traffic while net compression remains enabled. Process-wide counters
    // are intentionally not used here because heartbeat/internal RPC can legitimately use snappy and
    // share those counters. The dedicated repl.compression counters, however, must stay flat because
    // the marked replication data-plane connection negotiated uncompressed.
    rst.awaitReplication();
    const replCompressorBefore = getReplCompressionBytesIn(primary, "compressor");
    const replDecompressorBefore = getReplCompressionBytesIn(secondary, "decompressor");
    for (let i = 0; i < 5; ++i) {
        generateOplogTraffic(primary, "netcompress_scope", "c", i);
    }
    rst.awaitReplication();
    const replCompressorGrew = decompressorDelta(
        replCompressorBefore, getReplCompressionBytesIn(primary, "compressor"));
    const replDecompressorGrew = decompressorDelta(
        replDecompressorBefore, getReplCompressionBytesIn(secondary, "decompressor"));
    assert.eq(
        {},
        replCompressorGrew,
        "replicationNetworkCompression=disabled must keep the replication data-plane uncompressed " +
            "even when net compression is enabled; sync-source repl.compression compressor delta: " +
            tojson(replCompressorGrew),
    );
    assert.eq(
        {},
        replDecompressorGrew,
        "replicationNetworkCompression=disabled must keep the replication data-plane uncompressed " +
            "even when net compression is enabled; fetcher repl.compression decompressor delta: " +
            tojson(replDecompressorGrew),
    );

    // Heartbeats are internal, non-replication connections. If replication=disabled had leaked into
    // them, the replica set could not maintain its topology. A healthy set (stable primary, both
    // nodes reachable) demonstrates heartbeats continued to work under the net set.
    assert.commandWorked(primary.adminCommand({replSetGetStatus: 1}));
    assert.eq(2, rst.nodes.length);
    assert(rst.getPrimary(), "replica set lost its primary; heartbeats may have been disrupted");

    rst.stopSet();
})();

// Returns the compressor an EXTERNAL shell client (one that advertised `clientAdvertises` and is
// NOT an internal replica-set client) actually negotiated with the server at `host`, or the
// string "NONE" when the server offered nothing. db.hello().compression reflects the compressors
// negotiated during the connection handshake, so this observes the client-facing
// (net.compression.compressors) side independently of the replication channel. This is how the
// decoupling tests below prove that the union registry never leaks replication-only algorithms to
// external clients.
function externalClientNegotiatedCompression(host, clientAdvertises) {
    clearRawMongoProgramOutput();
    const rc = runMongoProgram(
        "mongo",
        "--host",
        host,
        "--networkMessageCompressors=" + clientAdvertises,
        "--eval",
        "const c = db.hello().compression; " +
            "print('NEGOTIATED_COMPRESSION=' + (c && c.length ? c.join(',') : 'NONE'));",
    );
    assert.eq(0, rc, `external client shell failed against ${host} (advertised ${clientAdvertises})`);
    const out = rawMongoProgramOutput(".*");
    const m = out.match(/NEGOTIATED_COMPRESSION=(\S+)/);
    assert(m, `could not find negotiation marker in shell output: ${out}`);
    return m[1];
}

// ---------------------------------------------------------------------------
// 7. Decoupling headline: net.compression.compressors DISABLED while replication
//    compression is ENABLED. This is the whole point of the feature and was previously impossible.
//    We prove three things at once:
//      (a) the node STARTS even though the only requested compressor comes from the replication
//          option (the algorithm is folded into the process-wide capability union at startup so it
//          gets registered), and
//      (b) the replication channel is compressed with the chosen algorithm, and
//      (c) external client-facing connections stay UNCOMPRESSED (net: disabled is still honored).
//
//    Note both nodes set replicationNetworkCompression: the secondary's fetcher must ADVERTISE the
//    algorithm, AND the primary (sync source) must ACCEPT it on the internal connection. If the
//    primary left the option at its default it would inherit net (disabled) for internal clients
//    and reject the offer, leaving the channel uncompressed.
// ---------------------------------------------------------------------------
(function testNetDisabledReplicationEnabledDecoupling() {
    jsTest.log.info("[replicationNetworkCompression] decoupling: net disabled, replication enabled");
    const kReplAlgo = "zstd";

    const rst = new ReplSetTest({
        name: "replication_network_compression_decouple",
        nodes: [
            {
                networkMessageCompressors: "disabled",
                setParameter: {replicationNetworkCompression: kReplAlgo},
            },
            {
                networkMessageCompressors: "disabled",
                setParameter: {replicationNetworkCompression: kReplAlgo},
            },
        ],
    });
    // (a) A failure here would mean the replication-only algorithm was not registered into the
    // union at startup (net was disabled), which is exactly the bug this feature fixes.
    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();
    const secondary = rst.getSecondary();
    rst.awaitReplication();

    // (b) Drive traffic and confirm the sync source's kReplAlgo COMPRESSOR grows, i.e. the
    // replication channel negotiated compression even though net.compression.compressors: disabled.
    // The sync source compresses the oplog batches it streams to the fetcher, so its compressor is
    // the reliable signal; its decompressor only sees the fetcher's tiny exhaust requests.
    const before = getCompressorBytesIn(primary);
    // Observability: the primary is the sync source, so it COMPRESSES oplog for the
    // fetcher. serverStatus().repl.compression.<algo>.compressor must grow for the replication
    // portion specifically (a subset of network.compression).
    const replBefore = getReplCompressionBytesIn(primary, "compressor");
    for (let i = 0; i < 5; ++i) {
        generateOplogTraffic(primary, "netcompress_decouple", "c", i);
    }
    rst.awaitReplication();
    const grew = decompressorDelta(before, getCompressorBytesIn(primary));
    jsTest.log.info("[replicationNetworkCompression] decouple compressor delta: " + tojson(grew));
    assert(
        grew[kReplAlgo] && grew[kReplAlgo] > 0,
        `expected replication channel to use '${kReplAlgo}' despite net disabled; delta: ${tojson(grew)}`,
    );

    // The dedicated repl.compression counter must attribute this to the replication data plane.
    const replGrew = decompressorDelta(replBefore, getReplCompressionBytesIn(primary, "compressor"));
    jsTest.log.info(
        "[replicationNetworkCompression] decouple repl.compression compressor delta: " +
            tojson(replGrew));
    assert(
        replGrew[kReplAlgo] && replGrew[kReplAlgo] > 0,
        `expected serverStatus().repl.compression.${kReplAlgo}.compressor to grow on the sync ` +
            `source; delta: ${tojson(replGrew)}`,
    );

    // (c) An external client that advertises the very same algorithm must NOT get it: with net
    // disabled the server offers nothing to a client-facing connection, so the union algorithm is
    // reachable by replication only.
    assert.eq(
        "NONE",
        externalClientNegotiatedCompression(secondary.host, kReplAlgo),
        "external client negotiated compression against a net-disabled node; net.compression." +
            "compressors: disabled must keep client-facing connections uncompressed",
    );

    rst.stopSet();
})();

// ---------------------------------------------------------------------------
// 8. Independent algorithm selection: net advertises ONE algorithm to clients,
//    while replication uses a DIFFERENT algorithm that is not in the net list at all. This proves
//    the two policies are chosen from independent candidate sets even though both algorithms share
//    a single process-wide capability union:
//      - the replication channel uses zstd (the sync source's zstd compressor grew), and
//      - an external client that advertises snappy gets snappy (net set), while an external client
//        that advertises zstd gets NOTHING (zstd is replication-only, never offered to clients).
// ---------------------------------------------------------------------------
(function testNetAndReplicationUseDifferentAlgorithms() {
    jsTest.log.info(
        "[replicationNetworkCompression] independent selection: net=snappy, replication=zstd");
    const kNetAlgo = "snappy";
    const kReplAlgo = "zstd";

    const rst = new ReplSetTest({
        name: "replication_network_compression_independent",
        nodes: [
            {
                networkMessageCompressors: kNetAlgo,
                setParameter: {replicationNetworkCompression: kReplAlgo},
            },
            {
                networkMessageCompressors: kNetAlgo,
                setParameter: {replicationNetworkCompression: kReplAlgo},
            },
        ],
    });
    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();
    const secondary = rst.getSecondary();
    rst.awaitReplication();

    // Replication channel must use zstd. Measure the sync source's COMPRESSOR: it compresses the
    // oplog batches it streams to the fetcher (the reverse decompressor only sees the fetcher's tiny
    // exhaust requests). Because net=snappy is still enabled here, the process-wide snappy compressor
    // counter can legitimately grow from heartbeats / other internal connections, so we do NOT assert
    // "snappy stayed flat" off that process-global counter (it would be flaky). The robust proof that
    // snappy is the net-only algorithm and zstd is replication-only is the pair of external-client
    // negotiation checks below.
    const before = getCompressorBytesIn(primary);
    const replBefore = getReplCompressionBytesIn(primary, "compressor");
    for (let i = 0; i < 5; ++i) {
        generateOplogTraffic(primary, "netcompress_independent", "c", i);
    }
    rst.awaitReplication();
    const grew = decompressorDelta(before, getCompressorBytesIn(primary));
    jsTest.log.info(
        "[replicationNetworkCompression] independent compressor delta on sync source: " +
            tojson(grew));
    assert(
        grew[kReplAlgo] && grew[kReplAlgo] > 0,
        `expected replication channel to use '${kReplAlgo}'; delta: ${tojson(grew)}`,
    );

    // The replication-data-plane subset must attribute the same growth to zstd specifically. This
    // pins that the replication channel (not net snappy) drove the zstd bytes.
    const replGrew = decompressorDelta(replBefore, getReplCompressionBytesIn(primary, "compressor"));
    assert(
        replGrew[kReplAlgo] && replGrew[kReplAlgo] > 0,
        `expected serverStatus().repl.compression.${kReplAlgo}.compressor to grow on the sync ` +
            `source; delta: ${tojson(replGrew)}`,
    );
    assert(
        !replGrew[kNetAlgo],
        `net-only algorithm '${kNetAlgo}' must not appear in the replication-data-plane counters; ` +
            `delta: ${tojson(replGrew)}`,
    );

    // External client advertising the net algorithm gets it (net set governs client-facing).
    assert.eq(
        kNetAlgo,
        externalClientNegotiatedCompression(secondary.host, kNetAlgo),
        `external client should negotiate '${kNetAlgo}' (in net.compression.compressors)`,
    );
    // External client advertising the replication-only algorithm gets NOTHING: even though zstd is
    // in the process-wide union (so replication can use it), it is never offered to a client-facing
    // connection because it is not in net.compression.compressors.
    assert.eq(
        "NONE",
        externalClientNegotiatedCompression(secondary.host, kReplAlgo),
        `replication-only algorithm '${kReplAlgo}' must not be offered to external clients`,
    );

    rst.stopSet();
})();

// ---------------------------------------------------------------------------
// 9. Initial (full) sync uses replication compression on the collection-cloner connection
//    The cloner connection now applies the exact same replicationNetworkCompression
//    policy as the steady-state oplog fetcher (shared helper applyReplicationNetworkCompressionToManager),
//    so full sync gets compressed just like incremental sync, and even works when
//    net.compression.compressors: disabled.
//
//    Signal: the bulk collection data flows sync source -> the syncing node as query responses, so
//    the SYNCING NODE decompresses it. The per-compressor decompressor.bytesIn counters are
//    process-global (MessageCompressorRegistry), so client-side (DBClientConnection) decompression
//    on the cloner connection is reflected in the syncing node's serverStatus. We seed several MB of
//    compressible data so the cloner payload dominates the tiny initial-sync oplog stream; a large
//    zstd decompressor delta on the fresh node therefore attributes to the cloner connection.
// ---------------------------------------------------------------------------
(function testInitialSyncUsesReplicationCompression() {
    jsTest.log.info(
        "[replicationNetworkCompression] initial sync: net disabled, replication=zstd on cloner");
    const kReplAlgo = "zstd";

    const rst = new ReplSetTest({
        name: "replication_network_compression_initial_sync",
        nodes: [
            {
                networkMessageCompressors: "disabled",
                setParameter: {replicationNetworkCompression: kReplAlgo},
            },
        ],
    });
    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();

    // Seed several MiB of NEAR-INCOMPRESSIBLE data BEFORE adding the syncing node. We deliberately
    // avoid a repeated filler: with highly compressible data the decompressor.bytesIn counter (which
    // measures COMPRESSED input bytes) would stay tiny and be indistinguishable from the small oplog
    // stream also fetched during initial sync. Random-ish base36 payloads keep the compressed volume
    // in the MiB range so a large decompressor.bytesIn delta can be attributed to the cloner.
    function randomPayload(len) {
        let s = "";
        while (s.length < len) {
            s += Math.random().toString(36).slice(2);
        }
        return s.substring(0, len);
    }
    const dbName = "initialsync_compress";
    const collName = "big";
    const bulk = primary.getDB(dbName)[collName].initializeUnorderedBulkOp();
    const kNumDocs = 3000;
    for (let i = 0; i < kNumDocs; ++i) {
        bulk.insert({_id: i, payload: randomPayload(2048)});
    }
    assert.commandWorked(bulk.execute());
    rst.awaitReplication();

    // Add a fresh node that must full-sync from the primary. Same config: net disabled so a client
    // connection would normally be uncompressed, but replicationNetworkCompression=zstd must still
    // compress the cloner connection.
    const newNode = rst.add({
        networkMessageCompressors: "disabled",
        setParameter: {replicationNetworkCompression: kReplAlgo},
        rsConfig: {votes: 0, priority: 0},
    });
    rst.reInitiate();
    rst.awaitSecondaryNodes(null, [newNode]);
    rst.awaitReplication();

    // A large zstd decompressor delta on the freshly-synced node proves the cloner connection
    // negotiated replication compression despite net.compression.compressors: disabled. Snapshot is
    // taken against an empty baseline since the node started fresh for this initial sync. We require
    // a threshold well above the few KiB the tiny initial-sync oplog stream could contribute, so the
    // signal is attributable to the multi-MiB collection-cloner payload rather than the oplog fetcher.
    const kMinClonerBytes = 256 * 1024;
    const grew = decompressorDelta({}, getDecompressorBytesIn(newNode));
    jsTest.log.info(
        "[replicationNetworkCompression] initial sync decompressor delta: " + tojson(grew));
    assert(
        grew[kReplAlgo] && grew[kReplAlgo] > kMinClonerBytes,
        `expected initial-sync cloner connection to use '${kReplAlgo}' despite net disabled ` +
            `(need > ${kMinClonerBytes} compressed bytes to attribute to the cloner, not the ` +
            `oplog fetcher); delta: ${tojson(grew)}`,
    );

    // Observability: the syncing node DECOMPRESSES the cloned collection data, so its
    // replication-only decompressor counter must also reflect it (subset of network.compression).
    const replGrew = decompressorDelta({}, getReplCompressionBytesIn(newNode, "decompressor"));
    jsTest.log.info(
        "[replicationNetworkCompression] initial sync repl.compression decompressor delta: " +
            tojson(replGrew));
    assert(
        replGrew[kReplAlgo] && replGrew[kReplAlgo] > kMinClonerBytes,
        `expected serverStatus().repl.compression.${kReplAlgo}.decompressor on the syncing node ` +
            `to grow from the cloner connection; delta: ${tojson(replGrew)}`,
    );

    // Sanity: the freshly-synced node actually cloned the data.
    assert.eq(
        kNumDocs,
        newNode.getDB(dbName)[collName].countDocuments({}),
        "initial-synced node is missing cloned documents",
    );

    rst.stopSet();
})();

// ---------------------------------------------------------------------------
// 10. Inheritance under net disabled: when replicationNetworkCompression is left
//     unset (its default value is the empty string ""), it INHERITS net.compression.compressors
//     rather than forcing any particular behavior of its own (see
//     parseReplicationNetworkCompression: "" -> inheritProcessDefault). Therefore, if net is
//     disabled AND the replication option is untouched, the replication channel must come up
//     UNCOMPRESSED. This pins the "empty == inherit" semantics end-to-end so that an operator who
//     never sets the replication option gets exactly the net behavior (here: no compression),
//     which the earlier scenarios (all run with the full net list) do not exercise.
// ---------------------------------------------------------------------------
(function testInheritFollowsNetDisabled() {
    jsTest.log.info(
        "[replicationNetworkCompression] inherit: net disabled + replication unset -> uncompressed");

    const rst = new ReplSetTest({
        name: "replication_network_compression_inherit_net_disabled",
        nodes: [
            // Both nodes disable net compression and leave replicationNetworkCompression at its
            // default (""), i.e. inherit the (disabled) net set on the replication connection too.
            {networkMessageCompressors: "disabled"},
            {networkMessageCompressors: "disabled"},
        ],
    });
    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();
    rst.awaitReplication();

    // Measure the sync source's COMPRESSOR: if the channel were (incorrectly) compressed the sync
    // source would compress the oplog it streams, growing this counter. Its decompressor only sees
    // the fetcher's tiny exhaust requests and would stay ~flat even for a compressed channel, which
    // would make this "must be uncompressed" assertion pass vacuously.
    const before = getCompressorBytesIn(primary);
    for (let i = 0; i < 5; ++i) {
        generateOplogTraffic(primary, "netcompress_inherit", "c", i);
    }
    rst.awaitReplication();
    const grew = decompressorDelta(before, getCompressorBytesIn(primary));
    jsTest.log.info(
        "[replicationNetworkCompression] inherit-under-net-disabled compressor delta: " +
            tojson(grew));
    assert.eq(
        {},
        grew,
        "with net.compression.compressors=disabled and replicationNetworkCompression left unset " +
            "(inherit), the replication channel must be uncompressed; delta: " + tojson(grew),
    );

    rst.stopSet();
})();

// ---------------------------------------------------------------------------
// 11. Asymmetric / disjoint allow-lists: the fetcher and its sync source advertise
//     DISJOINT replication allow-lists. With net disabled on both nodes, one node permits only
//     snappy on the replication connection and the other permits only zstd, so their candidate sets
//     do not intersect on the replication channel. serverNegotiate() then finds no mutually
//     permitted-and-registered algorithm and the connection must fall back SAFELY to uncompressed:
//     it must NOT error, must NOT pick a non-permitted algorithm, and replication must keep working.
//     (Note: this config-level mismatch is logged at DEBUG level 3 as "not permitted" (id 10130415)
//     on the sync source; it does NOT raise the rate-limited WARNING 10130417, which is reserved
//     for an algorithm that is permitted but not compiled into the build. Both algorithms here are
//     present in a standard build, so we assert the observable functional outcome instead.)
//     The disjoint pairing holds no matter which node is elected primary, since the intersection is
//     empty in either orientation, so we do not need to pin the topology.
// ---------------------------------------------------------------------------
(function testAsymmetricDisjointAllowListsFallBackUncompressed() {
    jsTest.log.info(
        "[replicationNetworkCompression] asymmetric: disjoint repl allow-lists -> uncompressed");

    const rst = new ReplSetTest({
        name: "replication_network_compression_asymmetric",
        nodes: [
            {
                networkMessageCompressors: "disabled",
                setParameter: {replicationNetworkCompression: "zstd"},
            },
            {
                networkMessageCompressors: "disabled",
                setParameter: {replicationNetworkCompression: "snappy"},
            },
        ],
    });
    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();
    const secondary = rst.getSecondary();
    rst.awaitReplication();

    // Measure the sync source's COMPRESSOR (it compresses the oplog it streams to the fetcher). A
    // successful negotiation would grow it; a correct fall-back-to-uncompressed leaves it flat.
    // The dedicated repl.compression counters must also stay flat for a disjoint negotiation.
    const before = getCompressorBytesIn(primary);
    const replCompressorBefore = getReplCompressionBytesIn(primary, "compressor");
    const replDecompressorBefore = getReplCompressionBytesIn(secondary, "decompressor");
    for (let i = 0; i < 5; ++i) {
        generateOplogTraffic(primary, "netcompress_asym", "c", i);
    }
    rst.awaitReplication();
    const grew = decompressorDelta(before, getCompressorBytesIn(primary));
    jsTest.log.info(
        "[replicationNetworkCompression] asymmetric compressor delta on sync source: " +
            tojson(grew));
    assert.eq(
        {},
        grew,
        "disjoint replication allow-lists (snappy vs zstd) must negotiate an UNCOMPRESSED channel " +
            "rather than pick a non-permitted algorithm; delta: " + tojson(grew),
    );
    const replCompressorGrew = decompressorDelta(
        replCompressorBefore, getReplCompressionBytesIn(primary, "compressor"));
    const replDecompressorGrew = decompressorDelta(
        replDecompressorBefore, getReplCompressionBytesIn(secondary, "decompressor"));
    assert.eq(
        {},
        replCompressorGrew,
        "disjoint allow-lists must leave sync-source repl.compression compressor counters flat; " +
            "delta: " + tojson(replCompressorGrew),
    );
    assert.eq(
        {},
        replDecompressorGrew,
        "disjoint allow-lists must leave fetcher repl.compression decompressor counters flat; " +
            "delta: " + tojson(replDecompressorGrew),
    );

    // A failed compression negotiation must not break replication: the set stays healthy and data
    // still flows (the traffic above already replicated via awaitReplication).
    assert.commandWorked(primary.adminCommand({replSetGetStatus: 1}));
    assert(rst.getPrimary(), "replica set lost its primary after a disjoint-allow-list negotiation");

    rst.stopSet();
})();

// ---------------------------------------------------------------------------
// 12. Rollback uses replication compression on the sync-source connection.
//     Rollback is a replication data-plane path too: to find the common point, the rolling-back
//     node reads its sync source's remote oplog over a dedicated DBClientConnection created in
//     BackgroundSync::_runRollback (via OplogInterfaceRemote). That connection now applies the same
//     node-local replicationNetworkCompression policy as the oplog fetcher and the initial-sync
//     cloner, so rollback traffic is compressed identically - and, in particular, honors
//     replication.networkCompression.compressors even when net.compression.compressors: disabled.
//
//     BRIDGE-FREE by design: the usual RollbackTest harness routes ALL replication through
//     mongobridge to create the partitions it needs, but mongobridge cannot relay a COMPRESSED
//     replication stream - it proxies the wire protocol and desynchronizes the request/response ids
//     on OP_COMPRESSED exhaust frames (bridge error Location50765 "Response ID did not match the
//     sent message ID"), so the bridged set never forms once replicationNetworkCompression is
//     enabled with net disabled. We therefore induce the rollback WITHOUT any network proxy, using
//     the stopReplProducer failpoint (stopServerReplication) plus a forced election. A single
//     arbiter gives the surviving data node a voting majority after the future-rollback node steps
//     down, so no partition is required.
//
//     Discriminating signal: with net disabled everywhere and replication=zstd, the ONLY way the
//     rolling-back node's zstd decompressor can advance is through a repl-compressed connection.
//     We isolate the rollback connection specifically from the (also-compressed) steady-state oplog
//     fetcher using two facts:
//       - the rolling-back node's producer is STALLED (stopServerReplication) while we stage the
//         divergent branches, so a baseline snapshot taken then finds the zstd decompressor
//         quiescent, and
//       - the `bgSyncHangAfterRunRollback` failpoint freezes the node the instant _runRollback()
//         returns - i.e. AFTER the common-point search read the remote oplog over the rollback
//         connection, but BEFORE the oplog fetcher resumes. A watcher shell snapshots the counter
//         at that frozen point (serverStatus still responds while the producer thread sleeps), then
//         releases the failpoint. The delta is therefore attributable to the rollback connection.
//     We seed many sync-source branch oplog entries so the common-point search moves well over the
//     threshold, comfortably above the tiny OplogStartMissing probe the fetcher issues just before
//     rollback begins. OplogInterfaceRemote projects remote oplog entries down to {ts, t}, so the
//     signal comes from entry count rather than application document payload size.
// ---------------------------------------------------------------------------
(function testRollbackUsesReplicationCompression() {
    jsTest.log.info(
        "[replicationNetworkCompression] rollback: net disabled, replication=zstd on rollback conn");
    const kReplAlgo = "zstd";
    // Well above what the pre-rollback OplogStartMissing fetch could contribute, so a delta past
    // this can only come from the common-point search reading the large sync-source branch. Keep the
    // threshold modest because the remote oplog query projects each oplog entry down to {ts, t}.
    const kMinRollbackRemoteBytes = 4 * 1024;

    const dataOpts = {
        networkMessageCompressors: "disabled",
        setParameter: {replicationNetworkCompression: kReplAlgo},
    };
    // 3-node set, NO mongobridge: two data nodes plus an arbiter. The arbiter lets the surviving
    // data node win the election after the future-rollback node steps down, so we can force the
    // divergence with failpoints instead of a network partition.
    const rst = new ReplSetTest({
        name: "replication_network_compression_rollback",
        nodes: [
            {rsConfig: {priority: 1}},        // node 0: starts primary, will roll back
            {rsConfig: {priority: 1}},        // node 1: becomes primary / rollback sync source
            {rsConfig: {arbiterOnly: true}},  // arbiter: sways the election without a bridge
        ],
        nodeOptions: dataOpts,
        settings: {
            // Disable catchup takeover. Node 0 accumulates the (uncommitted) doomed branch, making
            // it FRESHER than node 1; with catchup takeover enabled it would repeatedly re-seize the
            // primary from the freshly elected node 1 (vote denials "can see a healthy primary ... of
            // equal or greater priority"), so node 1 could never hold the primary long enough to
            // build the divergent branch node 0 must roll back.
            catchUpTakeoverDelayMillis: -1,
            // Disable new-primary catch-up. When node 1 is elected it is BEHIND node 0 (which holds
            // the doomed branch). With catch-up enabled the new primary would try to pull node 0's
            // fresher oplog before becoming writable - but node 1's producer is stalled
            // (stopServerReplication), so catch-up would stall and node 1 would never become a
            // writable primary, hanging ReplSetTest.stepUp() in an election-retry loop. Disabling
            // catch-up makes node 1 writable immediately AND ensures it never applies node 0's doomed
            // ops, which is exactly the divergence node 0 must roll back.
            catchUpTimeoutMillis: 0,
        },
    });
    rst.startSet();
    rst.initiate();

    const rollbackNode = rst.nodes[0];
    const syncSource = rst.nodes[1];

    // Make node 0 the initial primary (everything is caught up here, so a normal stepUp is fine).
    // Compare by host: getPrimary() may return a different connection object than rst.nodes[0], so an
    // object-identity (!==) check could wrongly stepUp again and perturb the election.
    if (rst.getPrimary().host !== rollbackNode.host) {
        rst.stepUp(rollbackNode);
    }
    waitForState(rollbackNode, ReplSetTest.State.PRIMARY);

    // Local (w:1) writes must commit without a majority so the doomed branch can form on node 0
    // alone. The default majority write concern would otherwise block once we stall node 1.
    assert.commandWorked(rollbackNode.adminCommand({
        setDefaultRWConcern: 1,
        defaultWriteConcern: {w: 1},
        writeConcern: {w: "majority"},
    }));
    assert.commandWorked(rollbackNode.getDB("rollback_compress").seed.insert(
        {_id: "seed"}, {writeConcern: {w: 2, wtimeout: ReplSetTest.kDefaultTimeoutMS}}));
    rst.awaitReplication();

    // Stall the future sync source so it never learns the doomed branch, and stall the rolling-back
    // node so it will NOT begin rollback until we have staged the full sync-source branch to read.
    stopServerReplication(syncSource);
    stopServerReplication(rollbackNode);

    // Doomed branch: ops that exist only on node 0 (w:1) and must be rolled back.
    {
        const bulk = rollbackNode.getDB("rollback_compress").doomed.initializeUnorderedBulkOp();
        for (let i = 0; i < 50; ++i) {
            bulk.insert({_id: `doomed-${i}`});
        }
        assert.commandWorked(bulk.execute({w: 1}));
    }

    // Step node 0 down (this also freezes it from elections) and elect node 1. Node 1 is behind (it
    // never saw the doomed branch) and is still stalled, but the arbiter gives it a voting majority
    // and stopReplProducer does not block elections. Use ReplSetTest.stepUp with
    // awaitReplicationBeforeStepUp:false: node 1 must NOT wait to catch up to node 0's doomed ops
    // (that divergence is exactly what we are creating). The helper retries replSetStepUp and waits
    // for the set to agree on the new primary, so it is robust against the election-timing races that
    // a hand-rolled replSetStepUp loop would hit (repeated stepUps can knock a freshly elected
    // primary back down).
    assert.commandWorked(rollbackNode.adminCommand({replSetStepDown: 300, force: true}));
    rst.stepUp(syncSource, {awaitReplicationBeforeStepUp: false});
    waitForState(syncSource, ReplSetTest.State.PRIMARY);
    // node 1 is primary now; clear its producer stall (harmless while primary) for a clean teardown.
    restartServerReplication(syncSource);

    // Sync-source branch: a large set of ops on the NEW primary's divergent branch. Node 0's
    // common-point search reads all of these back over the rollback connection - the traffic we
    // measure. OplogInterfaceRemote projects entries to {ts, t}, so volume comes from entry count.
    {
        const bulk = syncSource.getDB("rollback_compress").winner.initializeUnorderedBulkOp();
        for (let i = 0; i < 5000; ++i) {
            bulk.insert({_id: `winner-${i}`});
        }
        assert.commandWorked(bulk.execute({w: 1}));
    }

    // Baseline BEFORE node 0 resumes fetching and rolls back: its producer is still stalled, so the
    // zstd decompressor is quiescent.
    const before = getDecompressorBytesIn(rollbackNode);
    const replBefore = getReplCompressionBytesIn(rollbackNode, "decompressor");

    // Freeze the node right after the common-point search completes but before the fetcher resumes.
    assert.commandWorked(rollbackNode.adminCommand(
        {configureFailPoint: "bgSyncHangAfterRunRollback", mode: "alwaysOn"}));

    // Watcher: wait for the frozen point, snapshot the rollback node's compressed-bytes counter,
    // stash it on the (healthy) sync source, then release the failpoint so rollback can finish.
    //
    // IMPORTANT: rollback via recoverToStableTimestamp CLOSES the node's client connections while it
    // runs, so any single cached connection to the rolling-back node will throw a network error
    // mid-rollback. The watcher therefore reconnects with a FRESH Mongo() on every poll iteration and
    // tolerates transient failures, only succeeding once the node has hung at the post-rollback
    // failpoint (at which point it serves commands again and the counters reflect exactly the
    // common-point read done over the rollback connection, before the steady-state fetcher resumes).
    const awaitWatcher = startParallelShell(
        funWithArgs(
            function (rbPort, ssHost, algo) {
                const rbHost = "localhost:" + rbPort;
                let snapped = null;
                assert.soon(
                    () => {
                        try {
                            const rb = new Mongo(rbHost);
                            const res = rb.adminCommand({getLog: "global"});
                            if (!res.ok || !res.log ||
                                !res.log.some((line) => line.includes(
                                    "bgSyncHangAfterRunRollback failpoint is set"))) {
                                return false;
                            }
                            const ss = rb.adminCommand({serverStatus: 1});
                            if (!ss.ok) {
                                return false;
                            }
                            let bytes = 0;
                            if (ss.network && ss.network.compression &&
                                ss.network.compression[algo] &&
                                ss.network.compression[algo].decompressor) {
                                bytes = ss.network.compression[algo].decompressor.bytesIn;
                            }
                            let replBytes = 0;
                            if (ss.repl && ss.repl.compression && ss.repl.compression[algo] &&
                                ss.repl.compression[algo].decompressor) {
                                replBytes = ss.repl.compression[algo].decompressor.bytesIn;
                            }
                            snapped = {bytes: bytes, replBytes: replBytes};
                            return true;
                        } catch (e) {
                            // Rollback closes client connections; retry with a fresh connection.
                            return false;
                        }
                    },
                    "timed out waiting for bgSyncHangAfterRunRollback failpoint to engage",
                    5 * 60 * 1000,
                );
                try {
                    const src = new Mongo(ssHost);
                    assert.commandWorked(src.getDB("rollback_compress").probe.insert({
                        _id: "atRollbackHang",
                        bytes: snapped.bytes,
                        replBytes: snapped.replBytes,
                    }));
                } finally {
                    // Always release the rollback failpoint (resiliently), else the main thread would
                    // hang in awaitSecondaryNodes() until the test's global timeout.
                    assert.soon(
                        () => {
                            try {
                                const rb = new Mongo(rbHost);
                                return rb.adminCommand({
                                    configureFailPoint: "bgSyncHangAfterRunRollback",
                                    mode: "off",
                                }).ok === 1;
                            } catch (e) {
                                return false;
                            }
                        },
                        "failed to release bgSyncHangAfterRunRollback failpoint",
                        60 * 1000,
                    );
                }
            },
            rollbackNode.port,
            syncSource.host,
            kReplAlgo),
        null,
        true,
    );

    // Trigger the rollback WITHOUT a bridge: resume node 0's replication. It syncs from the new
    // primary, detects the divergent doomed branch (OplogStartMissing), and runs rollback - reading
    // the sync source's remote oplog over the compressed rollback connection, hitting the failpoint
    // (watcher snapshots + releases), then rejoining as a secondary.
    restartServerReplication(rollbackNode);
    awaitWatcher();
    rst.awaitSecondaryNodes(null, [rollbackNode]);
    rst.awaitReplication();

    const probe = syncSource.getDB("rollback_compress").probe.findOne({_id: "atRollbackHang"});
    assert(probe, "watcher did not record a rollback-hang decompressor snapshot");
    const delta = probe.bytes - (before[kReplAlgo] || 0);
    jsTest.log.info(
        `[replicationNetworkCompression] rollback connection zstd decompressor delta: ${delta}`);
    assert(
        delta > kMinRollbackRemoteBytes,
        `expected the rollback common-point connection to negotiate '${kReplAlgo}' despite ` +
            `net.compression.compressors: disabled (need > ${kMinRollbackRemoteBytes} compressed ` +
            `bytes read from the sync source's remote oplog), but delta was ${delta}. A zero/near-` +
            `zero delta means the rollback connection was left uncompressed (the gap ` +
            `in BackgroundSync::_runRollback).`,
    );

    const replDelta = probe.replBytes - (replBefore[kReplAlgo] || 0);
    jsTest.log.info(
        `[replicationNetworkCompression] rollback repl.compression zstd decompressor delta: ` +
            `${replDelta}`);
    assert(
        replDelta > kMinRollbackRemoteBytes,
        `expected serverStatus().repl.compression.${kReplAlgo}.decompressor on the rolling-back ` +
            `node to grow from the rollback common-point connection, but delta was ${replDelta}`,
    );

    // Data-consistency checks after a rollback performed under replication-only compression prove
    // the rollback itself completed correctly (functional coverage of the path), and that the doomed
    // branch was actually discarded.
    rst.awaitReplication();
    rst.checkReplicatedDataHashes();
    rst.checkOplogs();
    assert.eq(
        0,
        rollbackNode.getDB("rollback_compress").doomed.countDocuments({}),
        "doomed branch was not rolled back off node 0",
    );
    rst.stopSet();
})();

// ---------------------------------------------------------------------------
// 13. Both disabled: net.compression.compressors=disabled AND
//     replicationNetworkCompression=disabled on both nodes. Nothing on the node may negotiate
//     compression - neither the replication data channel nor client-facing connections. This is the
//     "fully uncompressed" corner of the net x repl matrix and complements the inherit-under-net-
//     disabled case (which reaches the same channel state through the default "" inherit path).
// ---------------------------------------------------------------------------
(function testBothDisabledUncompressedEverywhere() {
    jsTest.log.info(
        "[replicationNetworkCompression] both disabled: net=disabled + replication=disabled");

    const rst = new ReplSetTest({
        name: "replication_network_compression_both_disabled",
        nodes: [
            {
                networkMessageCompressors: "disabled",
                setParameter: {replicationNetworkCompression: "disabled"},
            },
            {
                networkMessageCompressors: "disabled",
                setParameter: {replicationNetworkCompression: "disabled"},
            },
        ],
    });
    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();
    const secondary = rst.getSecondary();
    rst.awaitReplication();

    // Replication channel must be uncompressed: the sync source's compressor must not grow from
    // fetcher traffic. The dedicated repl.compression counters must also stay flat.
    const before = getCompressorBytesIn(primary);
    const replCompressorBefore = getReplCompressionBytesIn(primary, "compressor");
    const replDecompressorBefore = getReplCompressionBytesIn(secondary, "decompressor");
    for (let i = 0; i < 5; ++i) {
        generateOplogTraffic(primary, "netcompress_both_disabled", "c", i);
    }
    rst.awaitReplication();
    const grew = decompressorDelta(before, getCompressorBytesIn(primary));
    jsTest.log.info(
        "[replicationNetworkCompression] both-disabled compressor delta on sync source: " +
            tojson(grew));
    assert.eq(
        {},
        grew,
        "with net.compression.compressors=disabled and replicationNetworkCompression=disabled the " +
            "replication channel must be uncompressed; delta: " + tojson(grew),
    );
    const replCompressorGrew = decompressorDelta(
        replCompressorBefore, getReplCompressionBytesIn(primary, "compressor"));
    const replDecompressorGrew = decompressorDelta(
        replDecompressorBefore, getReplCompressionBytesIn(secondary, "decompressor"));
    assert.eq(
        {},
        replCompressorGrew,
        "with both net and replication compression disabled, sync-source repl.compression " +
            "compressor counters must not grow; delta: " + tojson(replCompressorGrew),
    );
    assert.eq(
        {},
        replDecompressorGrew,
        "with both net and replication compression disabled, fetcher repl.compression decompressor " +
            "counters must not grow; delta: " + tojson(replDecompressorGrew),
    );

    // Client-facing connections must also be uncompressed: even advertising a real algorithm, an
    // external client gets nothing because net is disabled and replication=disabled adds nothing.
    assert.eq(
        "NONE",
        externalClientNegotiatedCompression(secondary.host, "zstd"),
        "external client negotiated compression while both net and replication compression are " +
            "disabled",
    );
    assert.eq(
        "NONE",
        externalClientNegotiatedCompression(secondary.host, "snappy"),
        "external client negotiated compression while both net and replication compression are " +
            "disabled",
    );

    rst.stopSet();
})();

// ---------------------------------------------------------------------------
// 14. CLI config path: the feature is exposed both as a setParameter
//     (replicationNetworkCompression) and as a config option
//     (replication.networkCompression.compressors, short name replicationNetworkCompressionCompressors,
//     source: [yaml, cli]). All earlier scenarios drive the setParameter form; this one exercises the
//     CLI/config bridge in storeMongodOptions(), which seeds gReplicationNetworkCompression from the
//     config value and folds the algorithm into the process-wide capability union. We bring up a set
//     with net disabled and the compressor supplied ONLY via the config option, and verify:
//       (a) getParameter reflects the seeded value (bridge wired), and
//       (b) the replication channel is actually compressed with that algorithm.
// ---------------------------------------------------------------------------
(function testConfigOptionPathCompresses() {
    jsTest.log.info(
        "[replicationNetworkCompression] CLI config path: replicationNetworkCompressionCompressors");
    const kReplAlgo = "zstd";

    const rst = new ReplSetTest({
        name: "replication_network_compression_config_path",
        nodes: [
            // Supply the algorithm via the CLI/config option (NOT --setParameter) on both nodes so
            // the fetcher advertises it and the sync source accepts it on the marked connection.
            {networkMessageCompressors: "disabled", replicationNetworkCompressionCompressors: kReplAlgo},
            {networkMessageCompressors: "disabled", replicationNetworkCompressionCompressors: kReplAlgo},
        ],
    });
    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();
    const secondary = rst.getSecondary();
    rst.awaitReplication();

    // (a) The config value must have been seeded into the (startup-only) setParameter.
    for (const node of [primary, secondary]) {
        const res = assert.commandWorked(
            node.adminCommand({getParameter: 1, replicationNetworkCompression: 1}));
        assert.eq(
            kReplAlgo,
            res.replicationNetworkCompression,
            `config option replicationNetworkCompressionCompressors was not seeded into the ` +
                `replicationNetworkCompression setParameter: ${tojson(res)}`,
        );
    }

    // (b) The replication channel must use the configured algorithm even though net is disabled.
    const before = getCompressorBytesIn(primary);
    const replBefore = getReplCompressionBytesIn(primary, "compressor");
    for (let i = 0; i < 5; ++i) {
        generateOplogTraffic(primary, "netcompress_config_path", "c", i);
    }
    rst.awaitReplication();
    const grew = decompressorDelta(before, getCompressorBytesIn(primary));
    jsTest.log.info(
        "[replicationNetworkCompression] config-path compressor delta on sync source: " +
            tojson(grew));
    assert(
        grew[kReplAlgo] && grew[kReplAlgo] > 0,
        `expected the config-supplied replication compressor '${kReplAlgo}' to be used on the ` +
            `replication channel; delta: ${tojson(grew)}`,
    );

    // The replication-data-plane subset must attribute the growth to the configured algorithm.
    const replGrew = decompressorDelta(replBefore, getReplCompressionBytesIn(primary, "compressor"));
    assert(
        replGrew[kReplAlgo] && replGrew[kReplAlgo] > 0,
        `expected serverStatus().repl.compression.${kReplAlgo}.compressor to grow on the sync ` +
            `source for the config-supplied compressor; delta: ${tojson(replGrew)}`,
    );

    rst.stopSet();
})();

// ---------------------------------------------------------------------------
// 15. Conflicting config + setParameter must be REJECTED: storeMongodOptions()
//     seeds the setParameter from replication.networkCompression.compressors and calls
//     checkConflictWithSetParameter(), so supplying BOTH the config option AND an initial
//     --setParameter replicationNetworkCompression is an ambiguous startup command and must fail
//     rather than silently letting one form win.
// ---------------------------------------------------------------------------
(function testConfigOptionConflictsWithSetParameterRejected() {
    jsTest.log.info(
        "[replicationNetworkCompression] config option + setParameter conflict is rejected");
    assert.throws(
        () => MongoRunner.runMongod({
            networkMessageCompressors: "disabled",
            replicationNetworkCompressionCompressors: "zstd",
            setParameter: {replicationNetworkCompression: "snappy"},
        }),
        [],
        "supplying both replication.networkCompression.compressors and the " +
            "replicationNetworkCompression setParameter must fail startup",
    );
})();

// ---------------------------------------------------------------------------
// 16. Unknown / not-compiled algorithm fails startup: a syntactically valid but
//     unregistered compressor name is folded into the process-wide capability union and then caught
//     by MessageCompressorRegistry::finalizeSupportedCompressors() (AllCompressorsRegistered
//     initializer), which uassertStatusOK()s a BadValue. This is a HARD startup error, not a silent
//     "dropped -> uncompressed" downgrade, for both the setParameter and the config/CLI forms.
// ---------------------------------------------------------------------------
(function testUnknownAlgorithmFailsStartup() {
    jsTest.log.info("[replicationNetworkCompression] unknown algorithm fails startup");
    // Not a MongoDB compressor (only noop/snappy/zlib/zstd are ever registered), so it can never be
    // present in any build - a stable "valid token, unregistered" input.
    const kBogusAlgo = "zstd_notacompressor";

    // setParameter form.
    assert.throws(
        () => MongoRunner.runMongod({setParameter: {replicationNetworkCompression: kBogusAlgo}}),
        [],
        `replicationNetworkCompression='${kBogusAlgo}' (setParameter) must fail startup, not be ` +
            `silently dropped`,
    );

    // config/CLI form.
    assert.throws(
        () => MongoRunner.runMongod({replicationNetworkCompressionCompressors: kBogusAlgo}),
        [],
        `replicationNetworkCompressionCompressors='${kBogusAlgo}' (config/CLI) must fail startup, ` +
            `not be silently dropped`,
    );
})();

// ---------------------------------------------------------------------------
// 17. Replication narrows to a SUBSET of a full net set. Both the net list and the
//     replication list share a single process-wide capability union; here net advertises the full
//     snappy,zstd,zlib set to clients while replication narrows to a single algorithm (zstd) that
//     is ALSO in the net set. This is the "repl is a subset of net" case (distinct from #8, where
//     the replication algorithm is NOT in the net set). We prove two independent facts:
//       (a) the replication channel negotiates exactly zstd - even though net ordering would pick
//           snappy first for any non-replication client - because the fetcher advertises only the
//           replication allow-list, and
//       (b) narrowing replication to zstd does NOT shrink the client-facing net set: external
//           clients advertising snappy or zlib (net members that are NOT the replication algorithm)
//           still negotiate them, proving the two policies are selected independently from the
//           shared union.
// ---------------------------------------------------------------------------
(function testReplicationNarrowsToSubsetOfFullNetSet() {
    jsTest.log.info(
        "[replicationNetworkCompression] subset: net=full union, replication narrows to zstd");
    const kReplAlgo = "zstd";

    const rst = new ReplSetTest({
        name: "replication_network_compression_subset_of_full_net",
        nodes: [
            {
                networkMessageCompressors: kProcessCompressors,
                setParameter: {replicationNetworkCompression: kReplAlgo},
            },
            {
                networkMessageCompressors: kProcessCompressors,
                setParameter: {replicationNetworkCompression: kReplAlgo},
            },
        ],
    });
    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();
    const secondary = rst.getSecondary();
    rst.awaitReplication();

    // (a) The replication channel used zstd. Because the fetcher advertises only its replication
    // allow-list ([zstd]), the sync source compresses the oplog it streams with zstd, so its zstd
    // compressor grows. zstd is in the net set too, but internal (non-replication) clients advertise
    // the net list in its configured order (snappy first), so heartbeats negotiate snappy - leaving
    // the zstd growth attributable to the replication fetcher.
    const before = getCompressorBytesIn(primary);
    for (let i = 0; i < 5; ++i) {
        generateOplogTraffic(primary, "netcompress_subset", "c", i);
    }
    rst.awaitReplication();
    const grew = decompressorDelta(before, getCompressorBytesIn(primary));
    jsTest.log.info(
        "[replicationNetworkCompression] subset compressor delta on sync source: " + tojson(grew));
    assert(
        grew[kReplAlgo] && grew[kReplAlgo] > 0,
        `expected the replication channel to narrow to '${kReplAlgo}' even though net offered the ` +
            `full set; delta: ${tojson(grew)}`,
    );
    // Pin it: the repl-marked fetcher connection negotiated EXACTLY [zstd], not snappy/zlib, despite
    // net advertising all three.
    assert.soon(
        () => hasReplicationCompressionNegotiationLog(secondary, true, [kReplAlgo]),
        `replication connection should negotiate exactly [${kReplAlgo}] when replication narrows a ` +
            `full net set to that subset`,
    );

    // (b) Narrowing replication to zstd must not remove the other net algorithms from the
    // client-facing set. External clients advertising snappy or zlib (net members other than the
    // replication algorithm) still negotiate them; if replication narrowing had wrongly shrunk the
    // client-facing net set to just zstd, these would return NONE.
    assert.eq(
        "snappy",
        externalClientNegotiatedCompression(secondary.host, "snappy"),
        "external client advertising snappy must still negotiate snappy: narrowing " +
            "replication to zstd must not shrink the client-facing net set",
    );
    assert.eq(
        "zlib",
        externalClientNegotiatedCompression(secondary.host, "zlib"),
        "external client advertising zlib must still negotiate zlib: narrowing " +
            "replication to zstd must not shrink the client-facing net set",
    );

    rst.stopSet();
})();

// ---------------------------------------------------------------------------
// 18. Ordered replication list is independent of a single (non-disabled) net algorithm
//     net advertises ONE algorithm to clients (snappy) while replication uses an
//     ORDERED list of two OTHER algorithms (zlib,zstd), neither present in the net set. Earlier
//     ordered-preference coverage (zstd_then_snappy) runs only with net disabled; this exercises
//     ordering while net compression stays ACTIVE. We prove:
//       (a) the replication channel picks the FIRST advertised algorithm (zlib) - client-advertised
//           order is the negotiation preference - and does NOT fall through to the second (zstd),
//       (b) neither replication-only algorithm is ever offered to external clients (only snappy is),
//           and
//       (c) an external client advertising snappy still negotiates snappy.
//     Because zlib/zstd are NOT in the net set, their compressor growth on the sync source can only
//     originate from the replication channel (heartbeats use snappy), giving a clean ordering signal.
// ---------------------------------------------------------------------------
(function testReplicationOrderedListIndependentOfSingleNetAlgo() {
    jsTest.log.info(
        "[replicationNetworkCompression] ordered: net=snappy, replication=zlib,zstd (prefers zlib)");
    const kNetAlgo = "snappy";
    const kReplFirst = "zlib";
    const kReplSecond = "zstd";
    const kReplList = `${kReplFirst},${kReplSecond}`;

    const rst = new ReplSetTest({
        name: "replication_network_compression_ordered_independent",
        nodes: [
            {
                networkMessageCompressors: kNetAlgo,
                setParameter: {replicationNetworkCompression: kReplList},
            },
            {
                networkMessageCompressors: kNetAlgo,
                setParameter: {replicationNetworkCompression: kReplList},
            },
        ],
    });
    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();
    const secondary = rst.getSecondary();
    rst.awaitReplication();

    // (a) The sync source compresses the oplog stream with the FIRST negotiated compressor (zlib).
    // zlib and zstd are not in the net set, so only the replication channel could grow either of
    // their compressors; a growing zlib with a flat zstd proves the client-advertised order won even
    // while net=snappy stayed enabled.
    const before = getCompressorBytesIn(primary);
    for (let i = 0; i < 5; ++i) {
        generateOplogTraffic(primary, "netcompress_ordered", "c", i);
    }
    rst.awaitReplication();
    const grew = decompressorDelta(before, getCompressorBytesIn(primary));
    jsTest.log.info(
        "[replicationNetworkCompression] ordered compressor delta on sync source: " + tojson(grew));
    assert(
        grew[kReplFirst] && grew[kReplFirst] > 0,
        `expected the replication channel to use the first-preference '${kReplFirst}'; delta: ` +
            `${tojson(grew)}`,
    );
    assert(
        !grew[kReplSecond],
        `second-preference '${kReplSecond}' must not have carried replication data while ` +
            `'${kReplFirst}' was available; delta: ${tojson(grew)}`,
    );
    // The channel is compressed (repl-marked connection negotiated a non-empty set).
    assert.soon(
        () => hasReplicationCompressionNegotiationLog(secondary, true),
        "replication connection should negotiate a compressed channel for an ordered repl list " +
            "even under a single non-disabled net algorithm",
    );

    // (b) The replication-only algorithms must never be offered to external clients.
    assert.eq(
        "NONE",
        externalClientNegotiatedCompression(secondary.host, kReplFirst),
        `replication-only algorithm '${kReplFirst}' must not be offered to external clients`,
    );
    assert.eq(
        "NONE",
        externalClientNegotiatedCompression(secondary.host, kReplSecond),
        `replication-only algorithm '${kReplSecond}' must not be offered to external clients`,
    );
    // (c) The net algorithm is still offered to clients.
    assert.eq(
        kNetAlgo,
        externalClientNegotiatedCompression(secondary.host, kNetAlgo),
        `external client advertising the net algorithm '${kNetAlgo}' must negotiate it`,
    );

    rst.stopSet();
})();

// ---------------------------------------------------------------------------
// 19. Inherit under a single net algorithm. replicationNetworkCompression is left
//     UNSET (default "" -> inherit net.compression.compressors). #4 covers inherit under the full
//     net set and #10 covers inherit under net disabled; this pins the remaining inherit corner: a
//     net set with exactly one algorithm (snappy). The replication channel must inherit and
//     negotiate that algorithm.
//
//     Signal: snappy is the ONLY process-wide algorithm here, so heartbeats also use snappy and the
//     process-wide snappy compressor counter cannot isolate the replication channel. We therefore
//     assert via the replication negotiation log (id 10130422), which is emitted ONLY for the
//     repl-marked fetcher connection and records that connection's negotiated compressors.
// ---------------------------------------------------------------------------
(function testInheritFollowsSingleNetAlgo() {
    jsTest.log.info(
        "[replicationNetworkCompression] inherit: net=snappy + replication unset -> snappy");
    const kNetAlgo = "snappy";

    const rst = new ReplSetTest({
        name: "replication_network_compression_inherit_single_net",
        nodes: [
            // Both nodes advertise a single net algorithm and leave replicationNetworkCompression at
            // its default (""), i.e. inherit the net set on the replication connection.
            {networkMessageCompressors: kNetAlgo},
            {networkMessageCompressors: kNetAlgo},
        ],
    });
    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();
    const secondary = rst.getSecondary();
    rst.awaitReplication();

    // The repl-marked fetcher connection inherited the net set and negotiated snappy.
    assert.soon(
        () => hasReplicationCompressionNegotiationLog(secondary, true, [kNetAlgo]),
        `inherit under net=${kNetAlgo}: the replication connection should negotiate ${kNetAlgo}`,
    );

    // Drive traffic and confirm the inherited algorithm is attributed to the replication data plane.
    const replCompressorBefore = getReplCompressionBytesIn(primary, "compressor");
    const replDecompressorBefore = getReplCompressionBytesIn(secondary, "decompressor");
    for (let i = 0; i < 5; ++i) {
        generateOplogTraffic(primary, "netcompress_inherit_single", "c", i);
    }
    rst.awaitReplication();
    assert.commandWorked(primary.adminCommand({replSetGetStatus: 1}));

    const replCompressorGrew = decompressorDelta(
        replCompressorBefore, getReplCompressionBytesIn(primary, "compressor"));
    const replDecompressorGrew = decompressorDelta(
        replDecompressorBefore, getReplCompressionBytesIn(secondary, "decompressor"));
    assert(
        replCompressorGrew[kNetAlgo] && replCompressorGrew[kNetAlgo] > 0,
        `expected inherited algorithm '${kNetAlgo}' to grow on the sync source's ` +
            `repl.compression compressor; delta: ${tojson(replCompressorGrew)}`,
    );
    assert(
        replDecompressorGrew[kNetAlgo] && replDecompressorGrew[kNetAlgo] > 0,
        `expected inherited algorithm '${kNetAlgo}' to grow on the fetcher's ` +
            `repl.compression decompressor; delta: ${tojson(replDecompressorGrew)}`,
    );

    rst.stopSet();
})();
