/**
 * Tests that replicationNetworkCompression controls compression on oplog fetching, initial sync
 * and rollback connections without changing net.compression.compressors for other connections.
 *
 * Oplog data flows from the sync source, so the sync source's compressor counters show what the
 * fetcher negotiated. For initial sync and rollback the syncing node's decompressor counters are
 * used instead.
 *
 * @tags: [requires_replication, requires_persistence]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {restartServerReplication, stopServerReplication} from "jstests/libs/write_concern_util.js";
import {waitForState} from "jstests/replsets/rslib.js";

const kProcessCompressors = "snappy,zstd,zlib";

// Returns {compressor name: compressor.bytesIn} from network.compression.
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

// Returns {compressor name: decompressor.bytesIn} from network.compression.
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

// Same as above for repl.compression. `direction` is "compressor" or "decompressor".
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

// Returns {name: delta} for the counters that grew.
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

function generateOplogTraffic(primary, dbName, collName, iteration) {
    const coll = primary.getDB(dbName)[collName];
    const bulk = coll.initializeUnorderedBulkOp();
    const filler = "x".repeat(1024);
    for (let i = 0; i < 200; ++i) {
        bulk.insert({_id: `${iteration}-${i}`, payload: filler});
    }
    assert.commandWorked(bulk.execute({w: 2}));
}

// `secondaryValue` is the secondary's replicationNetworkCompression (undefined leaves it unset).
// `expectCompressorGrew` is the compressor the sync source should use, or null for none.
function runScenario({label, secondaryValue, expectCompressorGrew, extraForbid}) {
    jsTest.log.info(`[replicationNetworkCompression] scenario: ${label}`);

    // network.compression is process-wide, so turn net compression off when checking a specific
    // replication compressor. Otherwise heartbeats would show up in the same counters.
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
        nodes: [primaryOpts, secondaryOpts],
    });
    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();
    const secondary = rst.getSecondary();

    rst.awaitReplication();
    const before = getCompressorBytesIn(primary);
    const replCompressorBefore = getReplCompressionBytesIn(primary, "compressor");
    const replDecompressorBefore = getReplCompressionBytesIn(secondary, "decompressor");

    for (let i = 0; i < 5; ++i) {
        generateOplogTraffic(primary, "netcompress", "c", i);
    }
    rst.awaitReplication();

    const after = getCompressorBytesIn(primary);
    const grew = decompressorDelta(before, after);
    const replCompressorGrew = decompressorDelta(
        replCompressorBefore,
        getReplCompressionBytesIn(primary, "compressor"),
    );
    const replDecompressorGrew = decompressorDelta(
        replDecompressorBefore,
        getReplCompressionBytesIn(secondary, "decompressor"),
    );
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

(function testStartupRejectsInvalidValues() {
    jsTest.log.info("[replicationNetworkCompression] startup rejects invalid values");

    assert.throws(() =>
        MongoRunner.runMongod({
            setParameter: {replicationNetworkCompression: "disabled,snappy"},
        }),
    );

    assert.throws(() =>
        MongoRunner.runMongod({
            setParameter: {replicationNetworkCompression: ","},
        }),
    );

    // Only "" means inherit.
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

(function testRuntimeSetParameterRejected() {
    jsTest.log.info("[replicationNetworkCompression] runtime setParameter is rejected");
    const startupValue = "zstd";
    const conn = MongoRunner.runMongod({
        networkMessageCompressors: kProcessCompressors,
        setParameter: {replicationNetworkCompression: startupValue},
    });
    const admin = conn.getDB("admin");

    let res = assert.commandWorked(
        admin.runCommand({getParameter: 1, replicationNetworkCompression: 1}),
    );
    assert.eq(startupValue, res.replicationNetworkCompression);

    // Includes the current value: the parameter is startup-only.
    for (const v of ["", "disabled", "snappy", "zstd,snappy", startupValue, "disabled,snappy"]) {
        const r = admin.runCommand({setParameter: 1, replicationNetworkCompression: v});
        assert.commandFailed(
            r,
            `runtime setParameter must be refused (startup-only) for value "${v}": ${tojson(r)}`,
        );
    }

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

// Unset: the fetcher offers the net compressors and the first one, snappy, is used.
runScenario({
    label: "default_inherits_process",
    secondaryValue: undefined,
    expectCompressorGrew: "snappy",
});

runScenario({
    label: "disabled",
    secondaryValue: "disabled",
    expectCompressorGrew: null,
});

runScenario({
    label: "only_zstd",
    secondaryValue: "zstd",
    expectCompressorGrew: "zstd",
    extraForbid: ["snappy", "zlib"],
});

runScenario({
    label: "only_zlib",
    secondaryValue: "zlib",
    expectCompressorGrew: "zlib",
    extraForbid: ["snappy", "zstd"],
});

// The first listed compressor is preferred.
runScenario({
    label: "zstd_then_snappy_prefers_zstd",
    secondaryValue: "zstd,snappy",
    expectCompressorGrew: "zstd",
    extraForbid: ["zlib"],
});

// Disabling replication compression leaves client compression alone, and client traffic is not
// counted in repl.compression.
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

    const replCompressorGrew = decompressorDelta(
        replCompressorBefore,
        getReplCompressionBytesIn(secondary, "compressor"),
    );
    const replDecompressorGrew = decompressorDelta(
        replDecompressorBefore,
        getReplCompressionBytesIn(secondary, "decompressor"),
    );
    assert.eq(
        {},
        replCompressorGrew,
        "external client response compression must not be counted in repl.compression compressor " +
            "counters; delta: " +
            tojson(replCompressorGrew),
    );
    assert.eq(
        {},
        replDecompressorGrew,
        "external client request compression must not be counted in repl.compression decompressor " +
            "counters; delta: " +
            tojson(replDecompressorGrew),
    );

    rst.stopSet();
})();

// With net=snappy and replication disabled, the oplog fetcher is uncompressed while heartbeats
// keep using the net compressors.
(function testReplicationCompressionDoesNotDisableInternalNonReplicationConnections() {
    jsTest.log.info(
        "[replicationNetworkCompression] scoping: replication=disabled must not touch heartbeats",
    );
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

    // Heartbeats use snappy too, so only repl.compression is checked here.
    rst.awaitReplication();
    const replCompressorBefore = getReplCompressionBytesIn(primary, "compressor");
    const replDecompressorBefore = getReplCompressionBytesIn(secondary, "decompressor");
    for (let i = 0; i < 5; ++i) {
        generateOplogTraffic(primary, "netcompress_scope", "c", i);
    }
    rst.awaitReplication();
    const replCompressorGrew = decompressorDelta(
        replCompressorBefore,
        getReplCompressionBytesIn(primary, "compressor"),
    );
    const replDecompressorGrew = decompressorDelta(
        replDecompressorBefore,
        getReplCompressionBytesIn(secondary, "decompressor"),
    );
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

    assert.commandWorked(primary.adminCommand({replSetGetStatus: 1}));
    assert.eq(2, rst.nodes.length);
    assert(rst.getPrimary(), "replica set lost its primary; heartbeats may have been disrupted");

    rst.stopSet();
})();

// Returns what a shell client offering `clientAdvertises` negotiates with `host`, or "NONE".
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
    assert.eq(
        0,
        rc,
        `external client shell failed against ${host} (advertised ${clientAdvertises})`,
    );
    const out = rawMongoProgramOutput(".*");
    const m = out.match(/NEGOTIATED_COMPRESSION=(\S+)/);
    assert(m, `could not find negotiation marker in shell output: ${out}`);
    return m[1];
}

// Net compression disabled, replication uses zstd. Both nodes need the setting: the fetcher offers
// zstd and the sync source has to accept it.
(function testNetDisabledReplicationEnabledDecoupling() {
    jsTest.log.info(
        "[replicationNetworkCompression] decoupling: net disabled, replication enabled",
    );
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
    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();
    const secondary = rst.getSecondary();
    rst.awaitReplication();

    const before = getCompressorBytesIn(primary);
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

    const replGrew = decompressorDelta(
        replBefore,
        getReplCompressionBytesIn(primary, "compressor"),
    );
    jsTest.log.info(
        "[replicationNetworkCompression] decouple repl.compression compressor delta: " +
            tojson(replGrew),
    );
    assert(
        replGrew[kReplAlgo] && replGrew[kReplAlgo] > 0,
        `expected serverStatus().repl.compression.${kReplAlgo}.compressor to grow on the sync ` +
            `source; delta: ${tojson(replGrew)}`,
    );

    assert.eq(
        "NONE",
        externalClientNegotiatedCompression(secondary.host, kReplAlgo),
        "external client negotiated compression against a net-disabled node; net.compression." +
            "compressors: disabled must keep client-facing connections uncompressed",
    );

    rst.stopSet();
})();

// net=snappy, replication=zstd: clients only get snappy and the fetcher uses zstd.
(function testNetAndReplicationUseDifferentAlgorithms() {
    jsTest.log.info(
        "[replicationNetworkCompression] independent selection: net=snappy, replication=zstd",
    );
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

    // Heartbeats use snappy, so the process-wide snappy counter is not checked.
    const before = getCompressorBytesIn(primary);
    const replBefore = getReplCompressionBytesIn(primary, "compressor");
    for (let i = 0; i < 5; ++i) {
        generateOplogTraffic(primary, "netcompress_independent", "c", i);
    }
    rst.awaitReplication();
    const grew = decompressorDelta(before, getCompressorBytesIn(primary));
    jsTest.log.info(
        "[replicationNetworkCompression] independent compressor delta on sync source: " +
            tojson(grew),
    );
    assert(
        grew[kReplAlgo] && grew[kReplAlgo] > 0,
        `expected replication channel to use '${kReplAlgo}'; delta: ${tojson(grew)}`,
    );

    const replGrew = decompressorDelta(
        replBefore,
        getReplCompressionBytesIn(primary, "compressor"),
    );
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

    assert.eq(
        kNetAlgo,
        externalClientNegotiatedCompression(secondary.host, kNetAlgo),
        `external client should negotiate '${kNetAlgo}' (in net.compression.compressors)`,
    );
    assert.eq(
        "NONE",
        externalClientNegotiatedCompression(secondary.host, kReplAlgo),
        `replication-only algorithm '${kReplAlgo}' must not be offered to external clients`,
    );

    rst.stopSet();
})();

// Initial sync clones over a zstd connection even with net compression disabled.
(function testInitialSyncUsesReplicationCompression() {
    jsTest.log.info(
        "[replicationNetworkCompression] initial sync: net disabled, replication=zstd on cloner",
    );
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

    // Random data, so the compressed size stays close to the 6 MiB that is cloned.
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

    // Initial sync starts fetching at the newest oplog entry, so keep that small instead of one of
    // the seed batches.
    assert.commandWorked(primary.getDB(dbName).marker.insert({_id: "beforeInitialSync"}));
    rst.awaitReplication();

    const newNode = rst.add({
        networkMessageCompressors: "disabled",
        setParameter: {replicationNetworkCompression: kReplAlgo},
        rsConfig: {votes: 0, priority: 0},
    });
    rst.reInitiate();
    rst.awaitSecondaryNodes(null, [newNode]);
    rst.awaitReplication();

    const kMinClonerBytes = 256 * 1024;
    const grew = decompressorDelta({}, getDecompressorBytesIn(newNode));
    jsTest.log.info(
        "[replicationNetworkCompression] initial sync decompressor delta: " + tojson(grew),
    );
    assert(
        grew[kReplAlgo] && grew[kReplAlgo] > kMinClonerBytes,
        `expected initial-sync cloner connection to use '${kReplAlgo}' despite net disabled ` +
            `(need > ${kMinClonerBytes} compressed bytes to attribute to the cloner, not the ` +
            `oplog fetcher); delta: ${tojson(grew)}`,
    );

    const replGrew = decompressorDelta({}, getReplCompressionBytesIn(newNode, "decompressor"));
    jsTest.log.info(
        "[replicationNetworkCompression] initial sync repl.compression decompressor delta: " +
            tojson(replGrew),
    );
    assert(
        replGrew[kReplAlgo] && replGrew[kReplAlgo] > kMinClonerBytes,
        `expected serverStatus().repl.compression.${kReplAlgo}.decompressor on the syncing node ` +
            `to grow from the cloner connection; delta: ${tojson(replGrew)}`,
    );

    assert.eq(
        kNumDocs,
        newNode.getDB(dbName)[collName].countDocuments({}),
        "initial-synced node is missing cloned documents",
    );

    rst.stopSet();
})();

// Unset replicationNetworkCompression with net compression disabled means no compression.
(function testInheritFollowsNetDisabled() {
    jsTest.log.info(
        "[replicationNetworkCompression] inherit: net disabled + replication unset -> uncompressed",
    );

    const rst = new ReplSetTest({
        name: "replication_network_compression_inherit_net_disabled",
        nodes: [{networkMessageCompressors: "disabled"}, {networkMessageCompressors: "disabled"}],
    });
    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();
    rst.awaitReplication();

    const before = getCompressorBytesIn(primary);
    for (let i = 0; i < 5; ++i) {
        generateOplogTraffic(primary, "netcompress_inherit", "c", i);
    }
    rst.awaitReplication();
    const grew = decompressorDelta(before, getCompressorBytesIn(primary));
    jsTest.log.info(
        "[replicationNetworkCompression] inherit-under-net-disabled compressor delta: " +
            tojson(grew),
    );
    assert.eq(
        {},
        grew,
        "with net.compression.compressors=disabled and replicationNetworkCompression left unset " +
            "(inherit), the replication channel must be uncompressed; delta: " +
            tojson(grew),
    );

    rst.stopSet();
})();

// zstd on one node and snappy on the other have nothing in common, so replication runs
// uncompressed. This holds whichever node becomes primary.
(function testAsymmetricDisjointAllowListsFallBackUncompressed() {
    jsTest.log.info(
        "[replicationNetworkCompression] asymmetric: disjoint repl allow-lists -> uncompressed",
    );

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
            tojson(grew),
    );
    assert.eq(
        {},
        grew,
        "disjoint replication allow-lists (snappy vs zstd) must negotiate an UNCOMPRESSED channel " +
            "rather than pick a non-permitted algorithm; delta: " +
            tojson(grew),
    );
    const replCompressorGrew = decompressorDelta(
        replCompressorBefore,
        getReplCompressionBytesIn(primary, "compressor"),
    );
    const replDecompressorGrew = decompressorDelta(
        replDecompressorBefore,
        getReplCompressionBytesIn(secondary, "decompressor"),
    );
    assert.eq(
        {},
        replCompressorGrew,
        "disjoint allow-lists must leave sync-source repl.compression compressor counters flat; " +
            "delta: " +
            tojson(replCompressorGrew),
    );
    assert.eq(
        {},
        replDecompressorGrew,
        "disjoint allow-lists must leave fetcher repl.compression decompressor counters flat; " +
            "delta: " +
            tojson(replDecompressorGrew),
    );

    assert.commandWorked(primary.adminCommand({replSetGetStatus: 1}));
    assert(
        rst.getPrimary(),
        "replica set lost its primary after a disjoint-allow-list negotiation",
    );

    rst.stopSet();
})();

// The rollback common point search reads the sync source's oplog over a zstd connection.
//
// mongobridge cannot relay compressed exhaust replies (Location50765), so RollbackTest is not
// used. The rollback is set up with stopServerReplication, a forced election and an arbiter.
//
// Only replication connections can use zstd here. To tell the rollback connection apart from the
// oplog fetcher:
//   - the sync source's branch is written one oplog entry per document, so the common point search
//     has to read thousands of {ts, t} entries;
//   - the fetcher batch size is 1, so the batch it reads before starting rollback is tiny;
//   - bgSyncHangAfterRunRollback stops the node before the fetcher restarts, and the counters are
//     read there.
(function testRollbackUsesReplicationCompression() {
    jsTest.log.info(
        "[replicationNetworkCompression] rollback: net disabled, replication=zstd on rollback conn",
    );
    const kReplAlgo = "zstd";
    const kNumSyncSourceEntries = 5000;
    const kMinRollbackRemoteBytes = 4 * 1024;

    const dataOpts = {
        networkMessageCompressors: "disabled",
        setParameter: {replicationNetworkCompression: kReplAlgo},
    };
    const rst = new ReplSetTest({
        name: "replication_network_compression_rollback",
        nodes: [
            {rsConfig: {priority: 1}}, // node 0: starts primary, will roll back
            {rsConfig: {priority: 1}}, // node 1: becomes primary / rollback sync source
            {rsConfig: {arbiterOnly: true}}, // arbiter: sways the election without a bridge
        ],
        nodeOptions: dataOpts,
        // Node 0 ends up ahead of node 1. Without these it would take the primary back, and node 1
        // would wait to catch up from a node it cannot replicate from.
        settings: {
            catchUpTakeoverDelayMillis: -1,
            catchUpTimeoutMillis: 0,
        },
    });
    rst.startSet();
    rst.initiate();

    const rollbackNode = rst.nodes[0];
    const syncSource = rst.nodes[1];

    if (rst.getPrimary().host !== rollbackNode.host) {
        rst.stepUp(rollbackNode);
    }
    waitForState(rollbackNode, ReplSetTest.State.PRIMARY);

    // The doomed branch is written with node 1 stopped, so it cannot wait for a majority.
    assert.commandWorked(
        rollbackNode.adminCommand({
            setDefaultRWConcern: 1,
            defaultWriteConcern: {w: 1},
            writeConcern: {w: "majority"},
        }),
    );
    assert.commandWorked(
        rollbackNode
            .getDB("rollback_compress")
            .seed.insert(
                {_id: "seed"},
                {writeConcern: {w: 2, wtimeout: ReplSetTest.kDefaultTimeoutMS}},
            ),
    );
    rst.awaitReplication();

    stopServerReplication(syncSource);
    stopServerReplication(rollbackNode);

    {
        const bulk = rollbackNode.getDB("rollback_compress").doomed.initializeUnorderedBulkOp();
        for (let i = 0; i < 50; ++i) {
            bulk.insert({_id: `doomed-${i}`});
        }
        assert.commandWorked(bulk.execute({w: 1}));
    }

    assert.commandWorked(rollbackNode.adminCommand({replSetStepDown: 300, force: true}));
    rst.stepUp(syncSource, {awaitReplicationBeforeStepUp: false});
    waitForState(syncSource, ReplSetTest.State.PRIMARY);
    restartServerReplication(syncSource);

    // A batched insert is written as a few applyOps entries; with a batch size of 1 each document
    // gets its own oplog entry.
    const insertBatchSize = assert.commandWorked(
        syncSource.adminCommand({getParameter: 1, internalInsertMaxBatchSize: 1}),
    ).internalInsertMaxBatchSize;
    assert.commandWorked(syncSource.adminCommand({setParameter: 1, internalInsertMaxBatchSize: 1}));
    {
        const bulk = syncSource.getDB("rollback_compress").winner.initializeUnorderedBulkOp();
        for (let i = 0; i < kNumSyncSourceEntries; ++i) {
            bulk.insert({_id: `winner-${i}`});
        }
        assert.commandWorked(bulk.execute({w: 1}));
    }
    assert.commandWorked(
        syncSource.adminCommand({setParameter: 1, internalInsertMaxBatchSize: insertBatchSize}),
    );

    const fetcherBatchSize = assert.commandWorked(
        rollbackNode.adminCommand({getParameter: 1, bgSyncOplogFetcherBatchSize: 1}),
    ).bgSyncOplogFetcherBatchSize;
    assert.commandWorked(
        rollbackNode.adminCommand({setParameter: 1, bgSyncOplogFetcherBatchSize: 1}),
    );

    const before = getDecompressorBytesIn(rollbackNode);
    const replBefore = getReplCompressionBytesIn(rollbackNode, "decompressor");

    assert.commandWorked(
        rollbackNode.adminCommand({
            configureFailPoint: "bgSyncHangAfterRunRollback",
            mode: "alwaysOn",
        }),
    );

    // Reads the counters while the node is stopped after rollback, saves them on the sync source and
    // lets rollback finish. Rollback closes client connections, so reconnect on every attempt.
    const awaitWatcher = startParallelShell(
        funWithArgs(
            function (rbPort, ssHost, algo, fetcherBatchSize) {
                const rbHost = "localhost:" + rbPort;
                let snapped = null;
                assert.soon(
                    () => {
                        try {
                            const rb = new Mongo(rbHost);
                            const res = rb.adminCommand({getLog: "global"});
                            if (
                                !res.ok ||
                                !res.log ||
                                !res.log.some((line) =>
                                    line.includes("bgSyncHangAfterRunRollback failpoint is set"),
                                )
                            ) {
                                return false;
                            }
                            const ss = rb.adminCommand({serverStatus: 1});
                            if (!ss.ok) {
                                return false;
                            }
                            let bytes = 0;
                            if (
                                ss.network &&
                                ss.network.compression &&
                                ss.network.compression[algo] &&
                                ss.network.compression[algo].decompressor
                            ) {
                                bytes = ss.network.compression[algo].decompressor.bytesIn;
                            }
                            let replBytes = 0;
                            if (
                                ss.repl &&
                                ss.repl.compression &&
                                ss.repl.compression[algo] &&
                                ss.repl.compression[algo].decompressor
                            ) {
                                replBytes = ss.repl.compression[algo].decompressor.bytesIn;
                            }
                            snapped = {bytes: bytes, replBytes: replBytes};
                            return true;
                        } catch (e) {
                            return false;
                        }
                    },
                    "timed out waiting for bgSyncHangAfterRunRollback failpoint to engage",
                    5 * 60 * 1000,
                );
                try {
                    const src = new Mongo(ssHost);
                    assert.commandWorked(
                        src.getDB("rollback_compress").probe.insert({
                            _id: "atRollbackHang",
                            bytes: snapped.bytes,
                            replBytes: snapped.replBytes,
                        }),
                    );
                } finally {
                    assert.soon(
                        () => {
                            try {
                                const rb = new Mongo(rbHost);
                                assert.commandWorked(
                                    rb.adminCommand({
                                        setParameter: 1,
                                        bgSyncOplogFetcherBatchSize: fetcherBatchSize,
                                    }),
                                );
                                return (
                                    rb.adminCommand({
                                        configureFailPoint: "bgSyncHangAfterRunRollback",
                                        mode: "off",
                                    }).ok === 1
                                );
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
            kReplAlgo,
            fetcherBatchSize,
        ),
        null,
        true,
    );

    restartServerReplication(rollbackNode);
    awaitWatcher();
    rst.awaitSecondaryNodes(null, [rollbackNode]);
    rst.awaitReplication();

    const probe = syncSource.getDB("rollback_compress").probe.findOne({_id: "atRollbackHang"});
    assert(probe, "watcher did not record a rollback-hang decompressor snapshot");
    const delta = probe.bytes - (before[kReplAlgo] || 0);
    jsTest.log.info(
        `[replicationNetworkCompression] rollback connection zstd decompressor delta: ${delta}`,
    );
    assert(
        delta > kMinRollbackRemoteBytes,
        `expected the rollback common-point connection to negotiate '${kReplAlgo}' despite ` +
            `net.compression.compressors: disabled (need > ${kMinRollbackRemoteBytes} compressed ` +
            `bytes read from the sync source's remote oplog), but delta was ${delta}`,
    );

    const replDelta = probe.replBytes - (replBefore[kReplAlgo] || 0);
    jsTest.log.info(
        `[replicationNetworkCompression] rollback repl.compression zstd decompressor delta: ` +
            `${replDelta}`,
    );
    assert(
        replDelta > kMinRollbackRemoteBytes,
        `expected serverStatus().repl.compression.${kReplAlgo}.decompressor on the rolling-back ` +
            `node to grow from the rollback common-point connection, but delta was ${replDelta}`,
    );

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

(function testBothDisabledUncompressedEverywhere() {
    jsTest.log.info(
        "[replicationNetworkCompression] both disabled: net=disabled + replication=disabled",
    );

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
            tojson(grew),
    );
    assert.eq(
        {},
        grew,
        "with net.compression.compressors=disabled and replicationNetworkCompression=disabled the " +
            "replication channel must be uncompressed; delta: " +
            tojson(grew),
    );
    const replCompressorGrew = decompressorDelta(
        replCompressorBefore,
        getReplCompressionBytesIn(primary, "compressor"),
    );
    const replDecompressorGrew = decompressorDelta(
        replDecompressorBefore,
        getReplCompressionBytesIn(secondary, "decompressor"),
    );
    assert.eq(
        {},
        replCompressorGrew,
        "with both net and replication compression disabled, sync-source repl.compression " +
            "compressor counters must not grow; delta: " +
            tojson(replCompressorGrew),
    );
    assert.eq(
        {},
        replDecompressorGrew,
        "with both net and replication compression disabled, fetcher repl.compression decompressor " +
            "counters must not grow; delta: " +
            tojson(replDecompressorGrew),
    );

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

// The same setting given as the replication.networkCompression.compressors option.
(function testConfigOptionPathCompresses() {
    jsTest.log.info(
        "[replicationNetworkCompression] CLI config path: replicationNetworkCompressionCompressors",
    );
    const kReplAlgo = "zstd";

    const rst = new ReplSetTest({
        name: "replication_network_compression_config_path",
        nodes: [
            {
                networkMessageCompressors: "disabled",
                replicationNetworkCompressionCompressors: kReplAlgo,
            },
            {
                networkMessageCompressors: "disabled",
                replicationNetworkCompressionCompressors: kReplAlgo,
            },
        ],
    });
    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();
    const secondary = rst.getSecondary();
    rst.awaitReplication();

    for (const node of [primary, secondary]) {
        const res = assert.commandWorked(
            node.adminCommand({getParameter: 1, replicationNetworkCompression: 1}),
        );
        assert.eq(
            kReplAlgo,
            res.replicationNetworkCompression,
            `config option replicationNetworkCompressionCompressors was not seeded into the ` +
                `replicationNetworkCompression setParameter: ${tojson(res)}`,
        );
    }

    const before = getCompressorBytesIn(primary);
    const replBefore = getReplCompressionBytesIn(primary, "compressor");
    for (let i = 0; i < 5; ++i) {
        generateOplogTraffic(primary, "netcompress_config_path", "c", i);
    }
    rst.awaitReplication();
    const grew = decompressorDelta(before, getCompressorBytesIn(primary));
    jsTest.log.info(
        "[replicationNetworkCompression] config-path compressor delta on sync source: " +
            tojson(grew),
    );
    assert(
        grew[kReplAlgo] && grew[kReplAlgo] > 0,
        `expected the config-supplied replication compressor '${kReplAlgo}' to be used on the ` +
            `replication channel; delta: ${tojson(grew)}`,
    );

    const replGrew = decompressorDelta(
        replBefore,
        getReplCompressionBytesIn(primary, "compressor"),
    );
    assert(
        replGrew[kReplAlgo] && replGrew[kReplAlgo] > 0,
        `expected serverStatus().repl.compression.${kReplAlgo}.compressor to grow on the sync ` +
            `source for the config-supplied compressor; delta: ${tojson(replGrew)}`,
    );

    rst.stopSet();
})();

(function testConfigOptionConflictsWithSetParameterRejected() {
    jsTest.log.info(
        "[replicationNetworkCompression] config option + setParameter conflict is rejected",
    );
    assert.throws(
        () =>
            MongoRunner.runMongod({
                networkMessageCompressors: "disabled",
                replicationNetworkCompressionCompressors: "zstd",
                setParameter: {replicationNetworkCompression: "snappy"},
            }),
        [],
        "supplying both replication.networkCompression.compressors and the " +
            "replicationNetworkCompression setParameter must fail startup",
    );
})();

(function testUnknownAlgorithmFailsStartup() {
    jsTest.log.info("[replicationNetworkCompression] unknown algorithm fails startup");
    const kBogusAlgo = "zstd_notacompressor";

    assert.throws(
        () => MongoRunner.runMongod({setParameter: {replicationNetworkCompression: kBogusAlgo}}),
        [],
        `replicationNetworkCompression='${kBogusAlgo}' (setParameter) must fail startup, not be ` +
            `silently dropped`,
    );

    assert.throws(
        () => MongoRunner.runMongod({replicationNetworkCompressionCompressors: kBogusAlgo}),
        [],
        `replicationNetworkCompressionCompressors='${kBogusAlgo}' (config/CLI) must fail startup, ` +
            `not be silently dropped`,
    );
})();

// Replication limited to zstd while net offers all three. Clients can still use the others.
(function testReplicationNarrowsToSubsetOfFullNetSet() {
    jsTest.log.info(
        "[replicationNetworkCompression] subset: net=full union, replication narrows to zstd",
    );
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

    // Heartbeats pick snappy, the first net compressor, so zstd only comes from replication.
    const before = getCompressorBytesIn(primary);
    for (let i = 0; i < 5; ++i) {
        generateOplogTraffic(primary, "netcompress_subset", "c", i);
    }
    rst.awaitReplication();
    const grew = decompressorDelta(before, getCompressorBytesIn(primary));
    jsTest.log.info(
        "[replicationNetworkCompression] subset compressor delta on sync source: " + tojson(grew),
    );
    assert(
        grew[kReplAlgo] && grew[kReplAlgo] > 0,
        `expected the replication channel to narrow to '${kReplAlgo}' even though net offered the ` +
            `full set; delta: ${tojson(grew)}`,
    );
    assert.soon(
        () => hasReplicationCompressionNegotiationLog(secondary, true, [kReplAlgo]),
        `replication connection should negotiate exactly [${kReplAlgo}] when replication narrows a ` +
            `full net set to that subset`,
    );

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

// net=snappy, replication=zlib,zstd: replication uses zlib, and clients never get zlib or zstd.
(function testReplicationOrderedListIndependentOfSingleNetAlgo() {
    jsTest.log.info(
        "[replicationNetworkCompression] ordered: net=snappy, replication=zlib,zstd (prefers zlib)",
    );
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

    const before = getCompressorBytesIn(primary);
    for (let i = 0; i < 5; ++i) {
        generateOplogTraffic(primary, "netcompress_ordered", "c", i);
    }
    rst.awaitReplication();
    const grew = decompressorDelta(before, getCompressorBytesIn(primary));
    jsTest.log.info(
        "[replicationNetworkCompression] ordered compressor delta on sync source: " + tojson(grew),
    );
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
    assert.soon(
        () => hasReplicationCompressionNegotiationLog(secondary, true),
        "replication connection should negotiate a compressed channel for an ordered repl list " +
            "even under a single non-disabled net algorithm",
    );

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
    assert.eq(
        kNetAlgo,
        externalClientNegotiatedCompression(secondary.host, kNetAlgo),
        `external client advertising the net algorithm '${kNetAlgo}' must negotiate it`,
    );

    rst.stopSet();
})();

// Unset replicationNetworkCompression with net=snappy. Heartbeats also use snappy, so this checks
// the fetcher's negotiation log and repl.compression instead of network.compression.
(function testInheritFollowsSingleNetAlgo() {
    jsTest.log.info(
        "[replicationNetworkCompression] inherit: net=snappy + replication unset -> snappy",
    );
    const kNetAlgo = "snappy";

    const rst = new ReplSetTest({
        name: "replication_network_compression_inherit_single_net",
        nodes: [{networkMessageCompressors: kNetAlgo}, {networkMessageCompressors: kNetAlgo}],
    });
    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();
    const secondary = rst.getSecondary();
    rst.awaitReplication();

    assert.soon(
        () => hasReplicationCompressionNegotiationLog(secondary, true, [kNetAlgo]),
        `inherit under net=${kNetAlgo}: the replication connection should negotiate ${kNetAlgo}`,
    );

    const replCompressorBefore = getReplCompressionBytesIn(primary, "compressor");
    const replDecompressorBefore = getReplCompressionBytesIn(secondary, "decompressor");
    for (let i = 0; i < 5; ++i) {
        generateOplogTraffic(primary, "netcompress_inherit_single", "c", i);
    }
    rst.awaitReplication();
    assert.commandWorked(primary.adminCommand({replSetGetStatus: 1}));

    const replCompressorGrew = decompressorDelta(
        replCompressorBefore,
        getReplCompressionBytesIn(primary, "compressor"),
    );
    const replDecompressorGrew = decompressorDelta(
        replDecompressorBefore,
        getReplCompressionBytesIn(secondary, "decompressor"),
    );
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
