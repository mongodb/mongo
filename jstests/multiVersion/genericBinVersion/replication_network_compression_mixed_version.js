/**
 * Replication keeps working when only one side knows replicationNetworkCompression. An old sync
 * source ignores the replicationCompressionClient hello field, and an old fetcher never sends it.
 * The parameter is only set on the latest binary, since last-lts would refuse to start.
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

function assertDataConsistent(rst, expectedCount) {
    for (const node of rst.nodes) {
        node.setSecondaryOk();
        const testDb = node.getDB(dbName);
        assert.soon(
            () => testDb[collName].countDocuments({}) === expectedCount,
            () =>
                `node ${node.host} never reached ${expectedCount} docs (had ` +
                `${testDb[collName].countDocuments({})})`,
        );
    }
}

function runMixedVersionScenario({label, primaryVersion, secondaryVersion, latestReplValue}) {
    jsTest.log.info(`[replNetCompress mixed] scenario: ${label}`);

    const makeNodeOpts = (binVersion, isPrimaryCandidate) => {
        const opts = {binVersion};
        if (binVersion === "latest" && latestReplValue !== undefined) {
            opts.setParameter = {replicationNetworkCompression: latestReplValue};
        }
        if (!isPrimaryCandidate) {
            opts.rsConfig = {priority: 0};
        }
        return opts;
    };

    const rst = new ReplSetTest({
        name: `repl_netcompress_mixed_${label}`,
        nodes: [makeNodeOpts(primaryVersion, true), makeNodeOpts(secondaryVersion, false)],
    });
    rst.startSet();
    rst.initiate();
    rst.awaitSecondaryNodes();

    const primary = rst.getPrimary();
    const secondary = rst.getSecondary();

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

    driveReplication(primary, rst, "steady");

    assert.commandWorked(primary.adminCommand({replSetGetStatus: 1}));
    assert(rst.getPrimary(), `[${label}] replica set lost its primary in a mixed-version set`);
    assertDataConsistent(rst, kNumDocs);

    const primaryVer = assert.commandWorked(primary.adminCommand({buildInfo: 1})).version;
    const secondaryVer = assert.commandWorked(secondary.adminCommand({buildInfo: 1})).version;
    jsTest.log.info(
        `[replNetCompress mixed] ${label}: primary=${primaryVer}, secondary=` + `${secondaryVer}`,
    );

    rst.stopSet();
}

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

runMixedVersionScenario({
    label: "old_primary_new_secondary_repl_disabled",
    primaryVersion: "last-lts",
    secondaryVersion: "latest",
    latestReplValue: "disabled",
});
