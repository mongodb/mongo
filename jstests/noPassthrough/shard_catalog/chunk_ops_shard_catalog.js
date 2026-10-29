/**
 * Verifies that after each chunk operation (split, mergeChunks, mergeAllChunks, and especially
 * moveRange across many scenarios), the shard's in-memory filtering metadata (CSR) and durable shard
 * catalog stay consistent with what the config server recorded. The guiding invariant is that a
 * shard's CSR must always be either correct or unknown — never stale/wrong.
 *
 * @tags: [
 *   featureFlagAuthoritativeShardsDDL,
 *   does_not_support_stepdowns,
 * ]
 */

import {configureFailPointForRS} from "jstests/libs/fail_point_util.js";
import {afterEach, after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// ------------------------------------------------------------
// Helper functions
// ------------------------------------------------------------

// Push every chunk's `onCurrentShardSince` outside the configsvr's snapshot history window,
// so mergeAllChunksOnShard considers them eligible for merging.
function setOnCurrentShardSince(mongoS, coll, extraQuery, refTimestamp, offsetInSeconds) {
    const session = mongoS.startSession({retryWrites: true});
    const sessionConfigDB = session.getDatabase("config");
    const collUuid = sessionConfigDB.collections.findOne({_id: coll.getFullName()}).uuid;
    const query = Object.assign({uuid: collUuid}, extraQuery);
    const newValue = new Timestamp(refTimestamp.getTime() + offsetInSeconds, 0);
    sessionConfigDB.chunks.find(query).forEach((chunk) => {
        assert.commandWorked(
            sessionConfigDB.chunks.updateOne({_id: chunk._id}, [
                {
                    $set: {
                        "onCurrentShardSince": newValue,
                        "history": [{validAfter: newValue, shard: "$shard"}],
                    },
                },
            ]),
        );
    });
}

function setHistoryWindowInSecs(st, valueInSeconds) {
    configureFailPointForRS(
        st.configRS.nodes,
        "overrideHistoryWindowInSecs",
        {seconds: valueInSeconds},
        "alwaysOn",
    );
    configureFailPointForRS(
        st.rs0.nodes,
        "overrideHistoryWindowInSecs",
        {seconds: valueInSeconds},
        "alwaysOn",
    );
    configureFailPointForRS(
        st.rs1.nodes,
        "overrideHistoryWindowInSecs",
        {seconds: valueInSeconds},
        "alwaysOn",
    );
    configureFailPointForRS(
        st.rs2.nodes,
        "overrideHistoryWindowInSecs",
        {seconds: valueInSeconds},
        "alwaysOn",
    );
}

function resetHistoryWindowInSecs(st) {
    configureFailPointForRS(st.configRS.nodes, "overrideHistoryWindowInSecs", {}, "off");
    configureFailPointForRS(st.rs0.nodes, "overrideHistoryWindowInSecs", {}, "off");
    configureFailPointForRS(st.rs1.nodes, "overrideHistoryWindowInSecs", {}, "off");
    configureFailPointForRS(st.rs2.nodes, "overrideHistoryWindowInSecs", {}, "off");
}

// Returns the collection's config entry, asserting it exists.
function getCollEntry(st, ns) {
    const collEntry = st.s.getDB("config").collections.findOne({_id: ns});
    assert(collEntry, "config.collections has no entry", {ns});
    return collEntry;
}

// Reads the shard's CSR placement version directly via getShardVersion against the shard
// primary (no router-injected shard version, so this read does not trigger an implicit
// stale-config refresh) and compares it — exact (major, minor) — to the highest
// lastmod on a chunk owned by that shard in config.chunks. The existing helper in
// jstests/libs/check_shard_filtering_metadata_helpers.js only checks the major
// component, which is insufficient: split/mergeChunks/mergeAllChunks bump only the
// minor component. Use this for a shard that OWNS at least one chunk.
function assertCsrMatchesConfig(st, ns, shardConn, shardName) {
    const collEntry = getCollEntry(st, ns);

    const topChunk = st.s
        .getDB("config")
        .chunks.find({uuid: collEntry.uuid, shard: shardName})
        .sort({lastmod: -1})
        .limit(1)
        .toArray()[0];
    assert(topChunk, "config.chunks has no chunk for this shard", {ns, shardName});

    const shardVersionRes = assert.commandWorked(
        shardConn.adminCommand({getShardVersion: ns, fullMetadata: true}),
    );
    assert(
        shardVersionRes.metadata && shardVersionRes.metadata.shardVersion,
        "getShardVersion returned no shardVersion",
        {ns, shardName, shardVersionRes},
    );
    const csrShardVersion = shardVersionRes.metadata.shardVersion;

    assert.eq(csrShardVersion.t, topChunk.lastmod.t, "Major version mismatch", {
        ns,
        shardName,
        csrShardVersion,
        configTop: topChunk.lastmod,
    });
    assert.eq(csrShardVersion.i, topChunk.lastmod.i, "Minor version mismatch", {
        ns,
        shardName,
        csrShardVersion,
        configTop: topChunk.lastmod,
    });
}

// For a shard that owns ZERO chunks after a move (e.g. a donor that gave up its last chunk): the CSR
// must be KNOWN and "tracked-unowned" — the collection version is known and equals config's, but the
// shard version is 0|0. It must NOT be UNKNOWN and must NOT be stale. Mirrors the assertions in
// jstests/sharding/catalog_shard_secondary_reads.js.
function assertCsrTrackedUnowned(st, ns, shardConn, shardName) {
    const collEntry = getCollEntry(st, ns);

    // The shard must own no chunks in config.
    const ownedChunks = st.s
        .getDB("config")
        .chunks.countDocuments({uuid: collEntry.uuid, shard: shardName});
    assert.eq(0, ownedChunks, "expected the shard to own no chunks", {ns, shardName});

    // The current collection version = highest lastmod across all of the collection's chunks.
    const topCollChunk = st.s
        .getDB("config")
        .chunks.find({uuid: collEntry.uuid})
        .sort({lastmod: -1})
        .limit(1)
        .toArray()[0];
    assert(topCollChunk, "config.chunks has no chunk for this collection", {ns});

    const shardVersionRes = assert.commandWorked(
        shardConn.adminCommand({getShardVersion: ns, fullMetadata: true}),
    );
    const metadata = shardVersionRes.metadata;
    assert(metadata, "getShardVersion returned no metadata (CSR is unknown)", {
        ns,
        shardName,
        shardVersionRes,
    });
    // Known collection version...
    assert(metadata.collVersion, "expected a known collection version (tracked-unowned)", {
        shardVersionRes,
    });
    assert.eq(
        0,
        timestampCmp(metadata.collVersion, topCollChunk.lastmod),
        "tracked-unowned collection version does not match config",
        {shardVersionRes, configTop: topCollChunk.lastmod},
    );
    // ...but the shard owns nothing, so its shard version is 0|0.
    assert.eq(
        0,
        timestampCmp(metadata.shardVersion, Timestamp(0, 0)),
        "expected shard version 0|0 (owns no chunks)",
        {shardVersionRes},
    );
}

// Asserts the shard's CSR is consistent with config regardless of whether it currently owns chunks:
// matches config if it owns any, tracked-unowned if it owns none. Use when the per-shard chunk count
// is not deterministic (e.g. hashed pre-split).
function assertCsrConsistent(st, ns, shardConn, shardName) {
    const collEntry = getCollEntry(st, ns);
    const ownsChunks =
        st.s.getDB("config").chunks.countDocuments({uuid: collEntry.uuid, shard: shardName}) > 0;
    if (ownsChunks) {
        assertCsrMatchesConfig(st, ns, shardConn, shardName);
    } else {
        assertCsrTrackedUnowned(st, ns, shardConn, shardName);
    }
}

// The strongest single guard: after every scenario, the config catalog, every shard's durable shard
// catalog, and every shard's in-memory CSR (primaries and secondaries) must agree. This directly
// catches the InconsistentShardCatalogCollectionMetadata class of bugs.
function assertMetadataConsistent(st, dbName) {
    const inconsistencies = st.s.getDB(dbName).checkMetadataConsistency().toArray();
    assert.eq(0, inconsistencies.length, "unexpected metadata inconsistencies", {inconsistencies});
}

// ------------------------------------------------------------
// Test cases
// ------------------------------------------------------------

describe("shard filtering metadata (CSR) matches config after chunk ops", function () {
    before(() => {
        this.st = new ShardingTest({
            mongos: 1,
            shards: 3,
            // Two nodes so checkMetadataConsistency's secondary fan-out validates secondary CSR.
            rs: {nodes: 2},
        });

        this.shard0Name = this.st.shard0.shardName;
        this.shard1Name = this.st.shard1.shardName;
        this.shard2Name = this.st.shard2.shardName;
        this.shard0Primary = this.st.rs0.getPrimary();
        this.shard1Primary = this.st.rs1.getPrimary();
        this.shard2Primary = this.st.rs2.getPrimary();

        this.dbCounter = 0;
    });

    after(() => {
        this.st.stop();
    });

    beforeEach(() => {
        // Unique db per case; shared dbs would let earlier ops contaminate later ones.
        this.dbName = `csrHealthDb_${this.dbCounter++}`;
        this.collName = "coll";
        this.ns = `${this.dbName}.${this.collName}`;

        assert.commandWorked(
            this.st.s.adminCommand({enableSharding: this.dbName, primaryShard: this.shard0Name}),
        );
        assert.commandWorked(this.st.s.adminCommand({shardCollection: this.ns, key: {x: 1}}));

        this.coll = this.st.s.getDB(this.dbName)[this.collName];
    });

    afterEach(() => {
        assert.commandWorked(this.st.s.getDB(this.dbName).dropDatabase());
    });

    it("split leaves donor CSR matching config", () => {
        // Single chunk (-inf, +inf) on shard0. Splitting at x=50 bumps the minor
        // version on shard0.
        assert.commandWorked(this.st.s.adminCommand({split: this.ns, middle: {x: 50}}));

        assertCsrMatchesConfig(this.st, this.ns, this.shard0Primary, this.shard0Name);
        assertMetadataConsistent(this.st, this.dbName);
    });

    it("mergeChunks leaves donor CSR matching config", () => {
        // Set up two adjacent chunks on shard0 around x=50, then merge them back.
        assert.commandWorked(this.st.s.adminCommand({split: this.ns, middle: {x: 50}}));

        assert.commandWorked(
            this.st.s.adminCommand({
                mergeChunks: this.ns,
                bounds: [{x: MinKey}, {x: MaxKey}],
            }),
        );

        assertCsrMatchesConfig(this.st, this.ns, this.shard0Primary, this.shard0Name);
        assertMetadataConsistent(this.st, this.dbName);
    });

    it("mergeAllChunks leaves shard CSR matching config", () => {
        // Set up several adjacent chunks on shard0.
        for (const middle of [10, 20, 30, 40, 50]) {
            assert.commandWorked(this.st.s.adminCommand({split: this.ns, middle: {x: middle}}));
        }

        // mergeAllChunksOnShard skips chunks inside the snapshot history window.
        // Push the window negative and rewrite onCurrentShardSince so every chunk
        // is eligible.
        setHistoryWindowInSecs(this.st, -10 * 60);
        try {
            setOnCurrentShardSince(
                this.st.s,
                this.coll,
                {shard: this.shard0Name},
                new Timestamp(100, 0),
                0,
            );

            assert.commandWorked(
                this.st.s.adminCommand({
                    mergeAllChunksOnShard: this.ns,
                    shard: this.shard0Name,
                }),
            );

            assertCsrMatchesConfig(this.st, this.ns, this.shard0Primary, this.shard0Name);
            assertMetadataConsistent(this.st, this.dbName);
        } finally {
            resetHistoryWindowInSecs(this.st);
        }
    });

    it("moveRange leaves donor and recipient CSRs matching config", () => {
        assert.commandWorked(
            this.st.s.adminCommand({
                moveRange: this.ns,
                min: {x: 50},
                toShard: this.shard1Name,
            }),
        );

        assertCsrMatchesConfig(this.st, this.ns, this.shard0Primary, this.shard0Name);
        assertCsrMatchesConfig(this.st, this.ns, this.shard1Primary, this.shard1Name);
        assertMetadataConsistent(this.st, this.dbName);
    });

    it("moveRange to a shard owning no chunks installs known metadata on the recipient", () => {
        // Two chunks on shard0; move the upper one to shard1, which owns nothing yet.
        assert.commandWorked(this.st.s.adminCommand({split: this.ns, middle: {x: 0}}));
        assert.commandWorked(
            this.st.s.adminCommand({
                moveRange: this.ns,
                min: {x: 0},
                toShard: this.shard1Name,
            }),
        );

        // The recipient primary's CSR is KNOWN (not left UNKNOWN) and matches config.
        assertCsrMatchesConfig(this.st, this.ns, this.shard1Primary, this.shard1Name);
        assertCsrMatchesConfig(this.st, this.ns, this.shard0Primary, this.shard0Name);

        // A direct write to the recipient for a key it now owns must succeed (a stale/UNKNOWN
        // recipient CSR would reject it with "not currently known", code 13388).
        assert.commandWorked(this.shard1Primary.getCollection(this.ns).insert({x: 5}));

        assertMetadataConsistent(this.st, this.dbName);
    });

    it("moveRange donating the last chunk leaves the donor tracked-unowned", () => {
        // Single chunk on shard0; move it whole to shard1 so shard0 ends owning nothing.
        assert.commandWorked(
            this.st.s.adminCommand({
                moveRange: this.ns,
                min: {x: MinKey},
                toShard: this.shard1Name,
            }),
        );

        assertCsrTrackedUnowned(this.st, this.ns, this.shard0Primary, this.shard0Name);
        assertCsrMatchesConfig(this.st, this.ns, this.shard1Primary, this.shard1Name);
        assertMetadataConsistent(this.st, this.dbName);
    });

    it("moveRange round-trip leaves CSRs consistent at every hop", () => {
        assert.commandWorked(this.st.s.adminCommand({split: this.ns, middle: {x: 0}}));

        // Hop 1: shard0 -> shard1. Both shards own a chunk.
        assert.commandWorked(
            this.st.s.adminCommand({
                moveRange: this.ns,
                min: {x: 0},
                toShard: this.shard1Name,
            }),
        );
        assertCsrMatchesConfig(this.st, this.ns, this.shard0Primary, this.shard0Name);
        assertCsrMatchesConfig(this.st, this.ns, this.shard1Primary, this.shard1Name);
        assertMetadataConsistent(this.st, this.dbName);

        // Hop 2: shard1 -> shard0. shard1 gives up its only chunk (tracked-unowned).
        assert.commandWorked(
            this.st.s.adminCommand({
                moveRange: this.ns,
                min: {x: 0},
                toShard: this.shard0Name,
            }),
        );
        assertCsrMatchesConfig(this.st, this.ns, this.shard0Primary, this.shard0Name);
        assertCsrTrackedUnowned(this.st, this.ns, this.shard1Primary, this.shard1Name);
        assertMetadataConsistent(this.st, this.dbName);
    });

    it("moveRange after a same-UUID generation change uses the new generation", () => {
        // Reproduces the moveCollection -> shardCollection sequence that keeps the same UUID but
        // assigns a new epoch/timestamp. A shard that learns the new placement only via a later
        // migration must not keep stale metadata from the old generation.
        const genCollName = "genColl";
        const genNs = `${this.dbName}.${genCollName}`;

        // Unsharded collection created on the db-primary (shard0) as unsplittable.
        assert.commandWorked(this.st.s.getDB(this.dbName).createCollection(genCollName));

        // Move it to shard1 (keeps the same UUID), then shard it (same UUID, new epoch/timestamp).
        assert.commandWorked(
            this.st.s.adminCommand({moveCollection: genNs, toShard: this.shard1Name}),
        );
        assert.commandWorked(this.st.s.adminCommand({shardCollection: genNs, key: {x: 1}}));
        assert.commandWorked(this.st.s.adminCommand({split: genNs, middle: {x: 0}}));

        // shard0 learns the new {x:1} placement only through this migration.
        assert.commandWorked(
            this.st.s.adminCommand({
                moveRange: genNs,
                min: {x: 0},
                toShard: this.shard0Name,
            }),
        );

        assertCsrMatchesConfig(this.st, genNs, this.shard0Primary, this.shard0Name);
        assertCsrMatchesConfig(this.st, genNs, this.shard1Primary, this.shard1Name);
        assertMetadataConsistent(this.st, this.dbName);
    });

    it("moveRange on a hashed shard key leaves CSRs consistent", () => {
        const hashedCollName = "hashedColl";
        const hashedNs = `${this.dbName}.${hashedCollName}`;
        // Hashed sharding pre-splits into multiple chunks spread across both shards.
        assert.commandWorked(
            this.st.s.adminCommand({shardCollection: hashedNs, key: {x: "hashed"}}),
        );

        // Move one of shard1's chunks back to shard0 (or vice versa); pick any existing chunk.
        const aChunk = this.st.s
            .getDB("config")
            .chunks.find({uuid: getCollEntry(this.st, hashedNs).uuid})
            .toArray()[0];
        assert(aChunk, "expected at least one chunk for the hashed collection", {ns: hashedNs});
        const toShard = aChunk.shard === this.shard0Name ? this.shard1Name : this.shard0Name;

        assert.commandWorked(
            this.st.s.adminCommand({
                moveRange: hashedNs,
                min: aChunk.min,
                max: aChunk.max,
                toShard,
            }),
        );

        assertCsrConsistent(this.st, hashedNs, this.shard0Primary, this.shard0Name);
        assertCsrConsistent(this.st, hashedNs, this.shard1Primary, this.shard1Name);
        assertMetadataConsistent(this.st, this.dbName);
    });
});
