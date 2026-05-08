/**
 * Verifies that after each chunk operation (split, mergeChunks, mergeAllChunks, and moveRange), the
 * shard's filtering metadata for the collection matches what the config server recorded.
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
    configureFailPointForRS(st.configRS.nodes, "overrideHistoryWindowInSecs", {seconds: valueInSeconds}, "alwaysOn");
}

function resetHistoryWindowInSecs(st) {
    configureFailPointForRS(st.configRS.nodes, "overrideHistoryWindowInSecs", {}, "off");
}

// Forces the CSR for `ns` on `shardConn` into the kNonAuthoritative state by clearing
// the in-memory filtering metadata via the test-only internal command. The CSR will be
// repopulated by the next versioned operation; if the authoritative-collection-metadata
// feature flag is on, that subsequent refresh may flip the CSR back to kAuthoritative —
// so this only guarantees the starting state immediately before the next op.
function forceCsrNonAuthoritative(shardConn, ns) {
    assert.commandWorked(
        shardConn.adminCommand({
            _internalClearCollectionShardingMetadata: ns,
            isAuthoritative: false,
        }),
    );
}

// Forces the CSR for `ns` on `shardConn` into the kAuthoritative state by running
// _shardsvrFetchCollMetadata, which fetches the latest metadata from the config server
// and installs it authoritatively on the shard. _shardsvrFetchCollMetadata requires
// migrations to be disabled, so we toggle setAllowMigrations off, run the command, then
// toggle migrations back on. Each toggle bumps the placement minor version; the chunk
// op under test triggers a refresh that brings the CSR back in sync with config, so the
// transient bumps don't affect the post-op assertion.
function forceCsrAuthoritative(st, shardConn, ns) {
    const setAllowMigrations = (allow) => {
        assert.commandWorked(
            st.configRS.getPrimary().adminCommand({
                _configsvrSetAllowMigrations: ns,
                allowMigrations: allow,
                writeConcern: {w: "majority"},
            }),
        );
    };

    setAllowMigrations(false);
    const session = shardConn.startSession({retryWrites: true});
    try {
        assert.commandWorked(
            session.getDatabase("admin").runCommand({
                _shardsvrFetchCollMetadata: ns,
                writeConcern: {w: "majority"},
                lsid: session.getSessionId(),
                txnNumber: NumberLong(1),
            }),
        );
    } finally {
        session.endSession();
    }
    setAllowMigrations(true);
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

    const shardVersionRes = assert.commandWorked(shardConn.adminCommand({getShardVersion: ns, fullMetadata: true}));
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

        assert.commandWorked(this.st.s.adminCommand({enableSharding: this.dbName, primaryShard: this.shard0Name}));
        assert.commandWorked(this.st.s.adminCommand({shardCollection: this.ns, key: {x: 1}}));

        this.coll = this.st.s.getDB(this.dbName)[this.collName];
    });

    afterEach(() => {
        assert.commandWorked(this.st.s.getDB(this.dbName).dropDatabase());
    });

    // Each chunk op runs twice: once with the shard's CSR forced into kAuthoritative via
    // _shardsvrFetchCollMetadata, and once with the CSR cleared to kNonAuthoritative via
    // the test-only _internalClearCollectionShardingMetadata command. Both starting states
    // must end with the CSR matching config after the chunk op.
    for (const startingState of ["Authoritative", "NonAuthoritative"]) {
        const forceShardCsrStartingState = (shardConn, ns) => {
            if (startingState === "Authoritative") {
                forceCsrAuthoritative(this.st, shardConn, ns);
            } else if (startingState === "NonAuthoritative") {
                forceCsrNonAuthoritative(shardConn, ns);
            }
        };

        it(`split leaves donor CSR matching config [${startingState}]`, () => {
            forceShardCsrStartingState(this.shard0Primary, this.ns);

            // Single chunk (-inf, +inf) on shard0. Splitting at x=50 bumps the minor
            // version on shard0.
            assert.commandWorked(this.st.s.adminCommand({split: this.ns, middle: {x: 50}}));

            assertCsrMatchesConfig(this.st, this.ns, this.shard0Primary, this.shard0Name);
        });

        it(`mergeChunks leaves donor CSR matching config [${startingState}]`, () => {
            // Set up two adjacent chunks on shard0 around x=50, then merge them back.
            assert.commandWorked(this.st.s.adminCommand({split: this.ns, middle: {x: 50}}));

            forceShardCsrStartingState(this.shard0Primary, this.ns);

            assert.commandWorked(
                this.st.s.adminCommand({
                    mergeChunks: this.ns,
                    bounds: [{x: MinKey}, {x: MaxKey}],
                }),
            );

            assertCsrMatchesConfig(this.st, this.ns, this.shard0Primary, this.shard0Name);
        });

        it(`mergeAllChunks leaves shard CSR matching config [${startingState}]`, () => {
            // Set up several adjacent chunks on shard0.
            for (const middle of [10, 20, 30, 40, 50]) {
                assert.commandWorked(this.st.s.adminCommand({split: this.ns, middle: {x: middle}}));
            }

            // mergeAllChunksOnShard skips chunks inside the snapshot history window.
            // Push the window negative and rewrite onCurrentShardSince so every chunk
            // is eligible.
            setHistoryWindowInSecs(this.st, -10 * 60);
            try {
                setOnCurrentShardSince(this.st.s, this.coll, {shard: this.shard0Name}, new Timestamp(100, 0), 0);

                forceShardCsrStartingState(this.shard0Primary, this.ns);

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

        it(`moveRange leaves donor and recipient CSRs matching config [${startingState}]`, () => {
            // Only flip the donor: the recipient owns no chunks for this collection yet,
            // so its CSR has nothing to install or clear. (For 'Authoritative',
            // _shardsvrFetchCollMetadata would tassert on the recipient since there are
            // no owned chunks to persist.) The recipient's CSR is installed by the
            // migration itself on commit.
            forceShardCsrStartingState(this.shard0Primary, this.ns);

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
    }
});
