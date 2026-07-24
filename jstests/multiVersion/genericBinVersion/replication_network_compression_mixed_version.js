/**
 * mixed-version (rolling upgrade) compatibility for the replication network
 * compression feature (replication.networkCompression.compressors / setParameter
 * replicationNetworkCompression).
 *
 * The feature is expressed on the wire by a NEW, optional, internal-stability hello field
 * "replicationCompressionClient" that only a "latest" oplog fetcher / initial-sync cloner sends,
 * and which tells the server to negotiate that connection against the replication candidate set
 * instead of net.compression.compressors. A "last-lts" binary does not know this field.
 *
 * This test proves the feature degrades SAFELY across a version boundary, i.e. it must not break
 * replication regardless of which side is old:
 *
 *   (1) latest secondary  <- last-lts primary (sync source is OLD):
 *       The latest node advertises "replicationCompressionClient" plus its replication allow-list,
 *       but the old sync source ignores the unknown field and simply negotiates the connection the
 *       way it always has (against its own net.compression.compressors). Replication must keep
 *       working and data must stay consistent. The replication channel may or may not be
 *       compressed, but it must never error out or stall.
 *
 *   (2) last-lts secondary <- latest primary (fetcher is OLD):
 *       The old fetcher never sends the marker, so the latest primary treats it as an ordinary
 *       internal connection and negotiates against net.compression.compressors, exactly as before
 *       this feature existed. Replication must keep working and data must stay consistent.
 *
 * Notes:
 *   - replicationNetworkCompression is a "latest"-only setParameter. It MUST NOT be passed to a
 *     last-lts binary (that binary would fail to start on the unknown parameter), so we only ever
 *     set it on the latest node.
 *   - We intentionally keep net.compression.compressors at its default (snappy,zstd,zlib) so the
 *     union registers all algorithms on the latest node and the old node negotiates normally; the
 *     point of this test is wire compatibility, not a specific negotiated algorithm.
 *
 * @tags: [
 *   requires_replication,
 *   requires_persistence,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

const dbName = "repl_netcompress_mixed";
const collName = "c";
const kNumDocs = 500;

// Push a batch of reasonably-sized documents through the primary and wait for the secondary to
// catch up, so the replication channel is actually exercised.
function driveReplication(primary, rst, tag) {
    const coll = primary.getDB(dbName)[collName];
    const bulk = coll.initializeUnorderedBulkOp();
    const filler = "x".repeat(1024);
    for (let i = 0; i < kNumDocs; ++i) {
        bulk.insert({_id: `${tag}-${i}`, payload: filler});
    }
    assert.commandWorked(bulk.execute({w: 2}));
    rst.awaitReplication();
}

// Assert both nodes agree on the document count for our test collection, i.e. replication actually
// delivered every write across the version boundary. Writes were done with w:2 and followed by
// awaitReplication(), so the secondary is already caught up; we still poll defensively. Reads are
// issued directly against each node's connection with secondaryOk so a secondary will serve them.
function assertDataConsistent(rst, expectedCount) {
    for (const node of rst.nodes) {
        const testDb = node.getDB(dbName);
        // getMongo().setSecondaryOk() is the version-portable way to allow reads on a secondary
        // from this shell connection.
        node.getMongo().setSecondaryOk();
        assert.soon(
            () => testDb[collName].countDocuments({}) === expectedCount,
            () => `node ${node.host} never reached ${expectedCount} docs (had ` +
                `${testDb[collName].countDocuments({})})`,
        );
    }
}

// Run one mixed-version orientation. `primaryVersion`/`secondaryVersion` are binVersions. Only the
// "latest" node (if any) receives replicationNetworkCompression, since a last-lts binary rejects
// the unknown parameter. `latestReplValue` is what to set on the latest node's replication
// compression option.
function runMixedVersionScenario({label, primaryVersion, secondaryVersion, latestReplValue}) {
    jsTest.log.info(`[replNetCompress mixed] scenario: ${label}`);

    const makeNodeOpts = (binVersion, isPrimaryCandidate) => {
        const opts = {binVersion};
        // Only the latest binary understands replicationNetworkCompression.
        if (binVersion === "latest" && latestReplValue !== undefined) {
            opts.setParameter = {replicationNetworkCompression: latestReplValue};
        }
        // Force the intended topology: the "primary" node keeps default priority, the other is
        // priority 0 so it cannot steal the primary role and flip our orientation.
        if (!isPrimaryCandidate) {
            opts.rsConfig = {priority: 0};
        }
        return opts;
    };

    const rst = new ReplSetTest({
        name: `repl_netcompress_mixed_${label}`,
        nodes: [
            makeNodeOpts(primaryVersion, true),
            makeNodeOpts(secondaryVersion, false),
        ],
    });
    rst.startSet();
    rst.initiate();
    rst.awaitSecondaryNodes();

    const primary = rst.getPrimary();
    const secondary = rst.getSecondary();

    // Sanity that the orientation is what we asked for (so the "sync source is OLD" vs
    // "fetcher is OLD" intent actually held and the test isn't silently trivial). node 0 was created
    // with primaryVersion and node 1 (priority 0) with secondaryVersion, so HARD-assert that the
    // elected primary is exactly node 0 and the secondary is node 1 before driving any writes. If a
    // stray election ever flipped the orientation we must fail loudly here rather than run a
    // version-swapped, silently meaningless scenario.
    assert.eq(
        primary.host,
        rst.nodes[0].host,
        `[${label}] expected node 0 (binVersion=${primaryVersion}) to be primary, but ` +
            `${primary.host} is - orientation flipped, the scenario would be invalid`,
    );
    assert.eq(
        secondary.host,
        rst.nodes[1].host,
        `[${label}] expected node 1 (binVersion=${secondaryVersion}) to be secondary, but ` +
            `${secondary.host} is - orientation flipped, the scenario would be invalid`,
    );

    // Baseline: some data already replicated during initiate; drive a fresh batch so we know the
    // steady-state oplog fetcher connection (whatever it negotiated) carries real traffic.
    driveReplication(primary, rst, "steady");

    // The whole point: replication must be healthy across the version boundary.
    assert.commandWorked(primary.adminCommand({replSetGetStatus: 1}));
    assert(rst.getPrimary(), `[${label}] replica set lost its primary in a mixed-version set`);

    // And the data must be identical on both the old and the new binary.
    assertDataConsistent(rst, kNumDocs);

    const primaryVer =
        assert.commandWorked(primary.adminCommand({buildInfo: 1})).version;
    const secondaryVer =
        assert.commandWorked(secondary.adminCommand({buildInfo: 1})).version;
    jsTest.log.info(`[replNetCompress mixed] ${label}: primary=${primaryVer}, secondary=` +
        `${secondaryVer}`);

    rst.stopSet();
}

// ---------------------------------------------------------------------------
// (1) Sync source is OLD: latest secondary fetching from a last-lts primary, with the latest node
//     requesting replication compression (zstd). The old primary must ignore the unknown
//     "replicationCompressionClient" hello field and keep replicating without error.
// ---------------------------------------------------------------------------
runMixedVersionScenario({
    label: "old_primary_new_secondary_repl_zstd",
    primaryVersion: "last-lts",
    secondaryVersion: "latest",
    latestReplValue: "zstd",
});

// ---------------------------------------------------------------------------
// (2) Fetcher is OLD: last-lts secondary fetching from a latest primary. The old fetcher never
//     advertises the marker, so the latest primary negotiates the internal connection against
//     net.compression.compressors just as it did before the feature. Setting replication
//     compression on the latest PRIMARY must not disturb the old secondary's replication.
// ---------------------------------------------------------------------------
runMixedVersionScenario({
    label: "new_primary_old_secondary_repl_zstd",
    primaryVersion: "latest",
    secondaryVersion: "last-lts",
    latestReplValue: "zstd",
});

// ---------------------------------------------------------------------------
// (3) Sync source is OLD, latest secondary explicitly DISABLES replication compression. This must
//     also degrade cleanly: the latest fetcher suppresses its compression offer, the old primary
//     sees a normal (uncompressed) internal connection, replication stays healthy.
// ---------------------------------------------------------------------------
runMixedVersionScenario({
    label: "old_primary_new_secondary_repl_disabled",
    primaryVersion: "last-lts",
    secondaryVersion: "latest",
    latestReplValue: "disabled",
});
