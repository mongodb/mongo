/**
 * SERVER-126178: pin parity between the connPoolStats command and the FTDC
 * connPoolStats collector. Both surfaces should expose the same top-level
 * key set, modulo a small list of intentional forks (FTDC drops `hosts` when
 * forFTDC=true; the command carries the standard reply envelope).
 *
 * This test fails today against the pre-unification code because:
 *   - FTDC reports `mongot` / `searchIndex` and the command does not.
 *   - The command reports replication-pool counters and FTDC does not (on
 *     replSet nodes; this test asserts the structural parity on a sharded
 *     fixture where the gap is observable via the `pools` map).
 *
 * After the SERVER-126178 unification this test passes without changes.
 *
 * @tags: [requires_sharding, requires_fcv_82]
 */
import {verifyGetDiagnosticData} from "jstests/libs/ftdc.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const testPath = MongoRunner.toRealPath("ftdc_parity_dir");
const st = new ShardingTest({
    shards: 2,
    mongos: {
        s0: {setParameter: {diagnosticDataCollectionDirectoryPath: testPath}},
    },
});

const admin = st.s0.getDB("admin");

// Pull both surfaces.
const cmdReply = assert.commandWorked(admin.runCommand({connPoolStats: 1}));
const ftdcReply = verifyGetDiagnosticData(admin).connPoolStats;

jsTestLog(`connPoolStats command keys: ${tojson(Object.keys(cmdReply).sort())}`);
jsTestLog(`FTDC connPoolStats keys:    ${tojson(Object.keys(ftdcReply).sort())}`);

// Keys that legitimately appear only on one side.
const CMD_ONLY_INTENTIONAL = new Set([
    "hosts",            // FTDC drops `hosts` when forFTDC=true (pinned by ftdc_connection_pool.js)
    "ok",               // command reply envelope
    "$clusterTime",     // command reply envelope
    "operationTime",    // command reply envelope
    "$configTime",      // command reply envelope, when present
    "$topologyTime",    // command reply envelope, when present
    "lastCommittedOpTime",
]);

const FTDC_ONLY_INTENTIONAL = new Set([
    // None today. The whole point of SERVER-126178 is that this set should
    // stay empty: anything FTDC reports that the command does not is a
    // divergence we want to surface, not paper over.
]);

const cmdKeys = new Set(Object.keys(cmdReply));
const ftdcKeys = new Set(Object.keys(ftdcReply));

const cmdOnly = [...cmdKeys].filter((k) => !ftdcKeys.has(k) && !CMD_ONLY_INTENTIONAL.has(k));
const ftdcOnly = [...ftdcKeys].filter((k) => !cmdKeys.has(k) && !FTDC_ONLY_INTENTIONAL.has(k));

assert.eq(cmdOnly,
          [],
          `connPoolStats command exposes top-level keys that FTDC does not (after ` +
              `excluding envelope/intentional forks): ${tojson(cmdOnly)}`);
assert.eq(ftdcOnly,
          [],
          `FTDC connPoolStats exposes top-level keys that the command does not: ` +
              `${tojson(ftdcOnly)}. This is the SERVER-126178 divergence.`);

// Additive parity expectations introduced by SERVER-126178: mongot and
// searchIndex diagnostics must be reachable from both surfaces. These keys
// are only present when the corresponding task executors are wired in; on a
// plain ShardingTest fixture they may or may not exist, so we only assert
// joint presence (both or neither), never asymmetric presence.
for (const k of ["mongot", "searchIndex"]) {
    assert.eq(cmdKeys.has(k),
              ftdcKeys.has(k),
              `Asymmetric presence of "${k}" subsection. command=${cmdKeys.has(k)}, ` +
                  `ftdc=${ftdcKeys.has(k)}. SERVER-126178 requires symmetric emission.`);
}

// Sanity: shared structural keys remain on both sides. Regression-pins so a
// future refactor cannot quietly drop one.
for (const k of ["pools", "replicaSetMonitor", "numClientConnections", "numAScopedConnections"]) {
    assert(cmdKeys.has(k), `command lost expected key "${k}"`);
    assert(ftdcKeys.has(k), `FTDC lost expected key "${k}"`);
}

st.stop();
