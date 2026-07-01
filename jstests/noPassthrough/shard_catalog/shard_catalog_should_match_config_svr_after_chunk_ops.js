/**
 * Verifies that after each chunk operation (split, mergeChunks, mergeAllChunks, and moveRange), the
 * shard's filtering metadata for the collection matches what the config server recorded.
 *
 * @tags: [
 *   requires_fcv_90,
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
}

function resetHistoryWindowInSecs(st) {
    configureFailPointForRS(st.configRS.nodes, "overrideHistoryWindowInSecs", {}, "off");
    configureFailPointForRS(st.rs0.nodes, "overrideHistoryWindowInSecs", {}, "off");
    configureFailPointForRS(st.rs1.nodes, "overrideHistoryWindowInSecs", {}, "off");
}

// Reads the shard's CSR placement version directly via getShardVersion against the shard
// primary (no router-injected shard version, so this read does not trigger an implicit
// stale-config refresh) and compares it — exact (major, minor) — to the highest
// lastmod on a chunk owned by that shard in config.chunks. The existing helper in
// jstests/libs/check_shard_filtering_metadata_helpers.js only checks the major
// component, which is insufficient: split/mergeChunks/mergeAllChunks bump only the
// minor component.
function assertCsrMatchesConfig(st, ns, shardConn, shardName) {
    const configDB = st.s.getDB("config");
    const collEntry = configDB.collections.findOne({_id: ns});
    assert(collEntry, `config.collections has no entry for ${ns}`);

    const topChunk = configDB.chunks
        .find({uuid: collEntry.uuid, shard: shardName})
        .sort({lastmod: -1})
        .limit(1)
        .toArray()[0];
    assert(topChunk, `config.chunks has no chunk for ${ns} on ${shardName}`);

    const shardVersionRes = assert.commandWorked(
        shardConn.adminCommand({getShardVersion: ns, fullMetadata: true}),
    );
    assert(
        shardVersionRes.metadata && shardVersionRes.metadata.shardVersion,
        `getShardVersion returned no shardVersion for ${ns} on ${shardName}: ${tojson(shardVersionRes)}`,
    );
    const csrShardVersion = shardVersionRes.metadata.shardVersion;

    assert.eq(
        csrShardVersion.t,
        topChunk.lastmod.t,
        `Major version mismatch for ${ns} on ${shardName}: csr=${tojson(csrShardVersion)} configTop=${tojson(topChunk.lastmod)}`,
    );
    assert.eq(
        csrShardVersion.i,
        topChunk.lastmod.i,
        `Minor version mismatch for ${ns} on ${shardName}: csr=${tojson(csrShardVersion)} configTop=${tojson(topChunk.lastmod)}`,
    );
}

// ------------------------------------------------------------
// Test cases
// ------------------------------------------------------------

describe("CSR health after chunk ops", function () {
    before(() => {
        this.st = new ShardingTest({
            mongos: 1,
            shards: 2,
            rs: {nodes: 1},
        });

        this.shard0Name = this.st.shard0.shardName;
        this.shard1Name = this.st.shard1.shardName;
        this.shard0Primary = this.st.rs0.getPrimary();
        this.shard1Primary = this.st.rs1.getPrimary();

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
                // Wait for orphan range deletion so the donor's post-cleanup CSR is
                // observable rather than a transient pre-cleanup snapshot.
                _waitForDelete: true,
            }),
        );

        assertCsrMatchesConfig(this.st, this.ns, this.shard0Primary, this.shard0Name);
        assertCsrMatchesConfig(this.st, this.ns, this.shard1Primary, this.shard1Name);
    });
});
