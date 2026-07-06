/**
 * Validates that FCV upgrade/downgrade of the authoritative shard catalog works for both database
 * and collection metadata, and does not block setFCV.
 *
 * TODO (SERVER-98118): Remove this test.
 *
 * @tags: [
 *   requires_fcv_90,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {afterEach, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const describeOrSkip = (() => {
    let st;
    try {
        st = new ShardingTest({shards: 1, mongos: 1, rs: {nodes: 1}});
        assert.commandWorked(
            st.s.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
        );

        const db = st.s.getDB("admin");
        if (
            FeatureFlagUtil.isPresentAndEnabled(db, "AuthoritativeShardsDDL") ||
            FeatureFlagUtil.isPresentAndEnabled(db, "AuthoritativeShardsCRUD")
        ) {
            jsTest.log.info(
                "Skipping test because AuthoritativeShards feature flags are already enabled in lastLTS",
            );
            return describe.skip;
        }
    } finally {
        if (st) {
            st.stop();
        }
    }

    return describe;
})();

const kAuthoritativeDb = "config";
const kAuthoritativeDbsColl = "shard.catalog.databases";
const kAuthoritativeCollectionsColl = "shard.catalog.collections";
const kAuthoritativeChunksColl = "shard.catalog.chunks";

function assertAuthoritativeShardCatalog(shard, {authoritativeCollections, haveDatabases}) {
    const configDB = shard.getDB(kAuthoritativeDb);
    const colls = configDB.getCollectionNames();
    assert.eq(authoritativeCollections, colls.includes(kAuthoritativeCollectionsColl), shard.host);
    assert.eq(authoritativeCollections, colls.includes(kAuthoritativeChunksColl), shard.host);
    if (authoritativeCollections) {
        assert.gt(
            configDB.getCollection(kAuthoritativeChunksColl).getIndexes().length,
            1,
            shard.host,
        );
    }
    // shard.catalog.databases is only created lazily once the shard owns databases
    assert.eq(haveDatabases, colls.includes(kAuthoritativeDbsColl), shard.host);
}

describeOrSkip("FCV lifecycle for authoritative metadata", function () {
    const kDbName = "testDb";

    let st, mongos, shard0, shard1, configPrimary;

    function assertFeatureFlags({enabled}) {
        const db = mongos.getDB(kDbName);
        assert.eq(
            enabled,
            FeatureFlagUtil.isPresentAndEnabled(db, "AuthoritativeShardsDDL"),
            `AuthoritativeShardsDDL flag should be ${enabled}`,
        );
        assert.eq(
            enabled,
            FeatureFlagUtil.isPresentAndEnabled(db, "AuthoritativeShardsCRUD"),
            `AuthoritativeShardsCRUD flag should be ${enabled}`,
        );
    }

    // Manually creates an authoritative shard catalog (databases + collections + chunks w/indexes) on
    // the given shard to simulate leftovers from a previously aborted upgrade.
    function createDirtyShardCatalog(shard, rs) {
        const configDB = shard.getDB(kAuthoritativeDb);
        assert.commandWorked(
            configDB.getCollection(kAuthoritativeDbsColl).insertOne({_id: kDbName}),
        );
        assert.commandWorked(configDB.createCollection(kAuthoritativeCollectionsColl));
        const chunks = configDB.getCollection(kAuthoritativeChunksColl);
        assert.commandWorked(chunks.createIndex({uuid: 1, min: 1}, {unique: true}));
        rs.awaitLastOpCommitted();
    }

    beforeEach(function () {
        st = new ShardingTest({shards: 2, mongos: 1, rs: {nodes: 1}});
        mongos = st.s;
        shard0 = st.shard0;
        shard1 = st.shard1;
        configPrimary = st.configRS.getPrimary();

        assert.commandWorked(
            mongos.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
        );

        assert.commandWorked(
            mongos.adminCommand({enableSharding: kDbName, primaryShard: st.shard0.shardName}),
        );
    });

    afterEach(function () {
        if (st) {
            st.stop();
        }
    });

    it("should correctly create and remove metadata on FCV upgrade and downgrade", function () {
        assertFeatureFlags({enabled: false});
        for (const node of [shard0, shard1, configPrimary]) {
            assertAuthoritativeShardCatalog(node, {
                authoritativeCollections: false,
                haveDatabases: false,
            });
        }

        // The upgrade creates the databases metadata on the primary shard, and the catalog on every shard.
        assert.commandWorked(
            mongos.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
        );

        assertFeatureFlags({enabled: true});
        assertAuthoritativeShardCatalog(shard0, {
            authoritativeCollections: true,
            haveDatabases: true,
        });
        for (const node of [shard1, configPrimary]) {
            assertAuthoritativeShardCatalog(node, {
                authoritativeCollections: true,
                haveDatabases: false,
            });
        }

        // The downgrade drops everything from every shard.
        assert.commandWorked(
            mongos.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
        );

        for (const node of [shard0, shard1, configPrimary]) {
            assertAuthoritativeShardCatalog(node, {
                authoritativeCollections: false,
                haveDatabases: false,
            });
        }
    });

    it("should drop pre-existing metadata collection on FCV upgrade", function () {
        assertFeatureFlags({enabled: false});
        assertAuthoritativeShardCatalog(shard0, {
            authoritativeCollections: false,
            haveDatabases: false,
        });

        // Manually create the authoritative shard catalog collections on shard0 to simulate a dirty state.
        createDirtyShardCatalog(shard0, st.rs0);
        assertAuthoritativeShardCatalog(shard0, {
            authoritativeCollections: true,
            haveDatabases: true,
        });

        // Dry-run should succeed and do not block on a dirty state.
        assert.commandWorked(
            mongos.adminCommand({setFeatureCompatibilityVersion: latestFCV, dryRun: true}),
            "Dry-run upgrade should have succeeded",
        );

        // The upgrade should succeed, dropping the bad collection on shard0 and re-creating it.
        assert.commandWorked(
            mongos.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
            "Actual upgrade should have succeeded",
        );

        assertFeatureFlags({enabled: true});
        assertAuthoritativeShardCatalog(shard0, {
            authoritativeCollections: true,
            haveDatabases: true,
        });
        assertAuthoritativeShardCatalog(shard1, {
            authoritativeCollections: true,
            haveDatabases: false,
        });
    });

    it("should drop pre-existing metadata collection on a non-primary shard during FCV upgrade", function () {
        assertFeatureFlags({enabled: false});
        assertAuthoritativeShardCatalog(shard1, {
            authoritativeCollections: false,
            haveDatabases: false,
        });

        // Manually create the authoritative shard catalog collections on a non-primary shard.
        createDirtyShardCatalog(shard1, st.rs1);
        assertAuthoritativeShardCatalog(shard1, {
            authoritativeCollections: true,
            haveDatabases: true,
        });

        // The upgrade should succeed, dropping the bad collection on shard1 and creating the
        // correct one on shard0.
        assert.commandWorked(
            mongos.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
            "Actual upgrade should have succeeded",
        );

        assertFeatureFlags({enabled: true});
        assertAuthoritativeShardCatalog(shard0, {
            authoritativeCollections: true,
            haveDatabases: true,
        });
        assertAuthoritativeShardCatalog(shard1, {
            authoritativeCollections: true,
            haveDatabases: false,
        });
    });

    it("should drop metadata collection from all shards on FCV downgrade", function () {
        assert.commandWorked(
            mongos.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
        );
        assertAuthoritativeShardCatalog(shard0, {
            authoritativeCollections: true,
            haveDatabases: true,
        });
        for (const node of [shard1, configPrimary]) {
            assertAuthoritativeShardCatalog(node, {
                authoritativeCollections: true,
                haveDatabases: false,
            });
        }

        // Manually create the databases collection on shard1 and the config server to simulate a
        // dirty state.
        for (const rs of [st.rs1, st.configRS]) {
            const node = rs.getPrimary();
            assert.commandWorked(
                node.getDB(kAuthoritativeDb).getCollection(kAuthoritativeDbsColl).insertOne({
                    _id: kDbName,
                }),
            );
            rs.awaitLastOpCommitted();
            assertAuthoritativeShardCatalog(node, {
                authoritativeCollections: true,
                haveDatabases: true,
            });
        }

        assert.commandWorked(
            mongos.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
        );

        // Assert everything is gone from all shards.
        assertFeatureFlags({enabled: false});
        for (const node of [shard0, shard1, configPrimary]) {
            assertAuthoritativeShardCatalog(node, {
                authoritativeCollections: false,
                haveDatabases: false,
            });
        }
    });

    function getGlobalCollectionMetadata(ns) {
        return mongos.getDB("config").collections.findOne({_id: ns});
    }

    function getGlobalChunksOnShard(uuid, shardName) {
        const query = {uuid, $or: [{shard: shardName}, {"history.shard": shardName}]};
        return mongos.getDB("config").chunks.find(query).sort({min: 1}).toArray();
    }

    // Asserts the shard's catalog holds exactly the metadata the global catalog says it owns. The
    // DB primary shard always carries an entry, even when it owns no chunks.
    function assertShardCatalogMatchesGlobal(shard, ns, {isDbPrimary}) {
        const meta = getGlobalCollectionMetadata(ns);
        const ownedChunks = getGlobalChunksOnShard(meta.uuid, shard.shardName);

        // The DB primary always keeps a collection entry even if it owns no chunks.
        const hasEntry = ownedChunks.length > 0 || isDbPrimary;
        const shardMeta = shard
            .getDB("config")
            .getCollection(kAuthoritativeCollectionsColl)
            .findOne({_id: ns});
        if (!hasEntry) {
            assert.eq(null, shardMeta, `${ns}: unexpected metadata on ${shard.shardName}`);
        } else {
            assert.neq(null, shardMeta, `${ns}: missing metadata on ${shard.shardName}`);
            assert.eq(
                meta.uuid.toString(),
                shardMeta.uuid.toString(),
                `${ns}: uuid mismatch on ${shard.shardName}`,
            );
            assert.eq(
                tojson(meta.key),
                tojson(shardMeta.key),
                `${ns}: shard key mismatch on ${shard.shardName}`,
            );
        }

        const shardChunks = shard
            .getDB("config")
            .getCollection(kAuthoritativeChunksColl)
            .find({uuid: meta.uuid})
            .sort({min: 1})
            .toArray();
        assert.eq(
            ownedChunks.length,
            shardChunks.length,
            `${ns}: chunk count mismatch on ${shard.shardName}`,
        );
        ownedChunks.forEach((chunk, i) => {
            assert.eq(
                tojson(chunk.min),
                tojson(shardChunks[i].min),
                `${ns}: chunk ${i} min mismatch`,
            );
            assert.eq(
                tojson(chunk.max),
                tojson(shardChunks[i].max),
                `${ns}: chunk ${i} max mismatch`,
            );
        });
    }

    it("should clone collection and chunk metadata to owning shards on FCV upgrade", function () {
        const db = mongos.getDB(kDbName);

        // A sharded collection with one chunk on each shard.
        const shardedNs = `${kDbName}.sharded`;
        assert.commandWorked(db.adminCommand({shardCollection: shardedNs, key: {x: 1}}));
        assert.commandWorked(db.adminCommand({split: shardedNs, middle: {x: 0}}));
        assert.commandWorked(
            db.adminCommand({
                moveChunk: shardedNs,
                find: {x: 0},
                to: shard1.shardName,
                _waitForDelete: true,
            }),
        );

        // A tracked unsplittable collection living on the non-primary shard.
        const trackedNs = `${kDbName}.tracked`;
        assert.commandWorked(
            db.runCommand({createUnsplittableCollection: "tracked", dataShard: shard1.shardName}),
        );

        // Nothing is cloned into the shard catalogs while still on lastLTS.
        assert.eq(
            null,
            shard0
                .getDB("config")
                .getCollection(kAuthoritativeCollectionsColl)
                .findOne({_id: shardedNs}),
        );
        assert.eq(
            null,
            shard1
                .getDB("config")
                .getCollection(kAuthoritativeCollectionsColl)
                .findOne({_id: shardedNs}),
        );

        assert.commandWorked(
            mongos.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
        );
        st.awaitReplicationOnShards();

        assertShardCatalogMatchesGlobal(shard0, shardedNs, {isDbPrimary: true});
        assertShardCatalogMatchesGlobal(shard1, shardedNs, {isDbPrimary: false});

        assertShardCatalogMatchesGlobal(shard1, trackedNs, {isDbPrimary: false});
        assertShardCatalogMatchesGlobal(shard0, trackedNs, {isDbPrimary: true});
    });

    it("should clone config.system.sessions collection metadata to owning shards on FCV upgrade", function () {
        const sessionsNs = "config.system.sessions";

        let sessionsMetadata;
        assert.soon(() => {
            assert.commandWorked(mongos.adminCommand({refreshLogicalSessionCacheNow: 1}));
            sessionsMetadata = getGlobalCollectionMetadata(sessionsNs);
            return sessionsMetadata != null;
        }, "config.system.sessions was not tracked in the global catalog in time");

        // Place the collection's data on a data shard that is not the primary of the config database
        // (the config server), so the upgrade must clone the metadata to a chunk-owning non-primary
        // shard.
        const sessionsChunk = mongos.getDB("config").chunks.findOne({uuid: sessionsMetadata.uuid});
        assert.neq(
            null,
            sessionsChunk,
            "config.system.sessions is expected to have at least one chunk",
        );
        if (sessionsChunk.shard !== shard1.shardName) {
            assert.commandWorked(
                mongos.adminCommand({
                    moveChunk: sessionsNs,
                    bounds: [sessionsChunk.min, sessionsChunk.max],
                    to: shard1.shardName,
                    _waitForDelete: true,
                }),
            );
        }

        // Nothing is cloned into the shard catalogs while still on lastLTS.
        assert.eq(
            null,
            shard1
                .getDB("config")
                .getCollection(kAuthoritativeCollectionsColl)
                .findOne({_id: sessionsNs}),
        );

        assert.commandWorked(
            mongos.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
        );
        st.awaitReplicationOnShards();

        // The config database primary is the config server, so the data shards are never its DB
        // primary; each data shard must carry the authoritative metadata exactly when it owns chunks.
        assertShardCatalogMatchesGlobal(shard1, sessionsNs, {isDbPrimary: false});
        assertShardCatalogMatchesGlobal(shard0, sessionsNs, {isDbPrimary: false});

        // A correct upgrade must leave no shard catalog inconsistencies for the collection.
        const inconsistencies = mongos.getDB("admin").checkMetadataConsistency().toArray();
        const shardCatalogInconsistencies = inconsistencies.filter(
            (i) => i.type === "InconsistentShardCatalogCollectionMetadata",
        );
        assert.eq(
            0,
            shardCatalogInconsistencies.length,
            () => `Unexpected shard catalog inconsistencies: ${tojson(inconsistencies)}`,
        );
    });

    // Pauses the donor's cloning DDL mid-upgrade, moves the database to the destination while paused,
    // then resumes it. In this case the movePrimary, rather than the cloning DDL, becomes responsible
    // for making the collection metadata authoritative.
    function runMovePrimaryRaceDuringUpgrade(donorShard, destShard) {
        const dbName = "movePrimaryRaceDb";
        const db = mongos.getDB(dbName);
        assert.commandWorked(
            db.adminCommand({enableSharding: dbName, primaryShard: donorShard.shardName}),
        );

        // A collection whose only chunk stays on the donor.
        const onDonorNs = `${dbName}.onDonor`;
        assert.commandWorked(db.adminCommand({shardCollection: onDonorNs, key: {x: 1}}));

        // A collection whose only chunk is moved onto the destination.
        const movedNs = `${dbName}.movedToNewPrimary`;
        assert.commandWorked(db.adminCommand({shardCollection: movedNs, key: {x: 1}}));
        assert.commandWorked(
            db.adminCommand({
                moveChunk: movedNs,
                find: {x: 0},
                to: destShard.shardName,
                _waitForDelete: true,
            }),
        );

        // A collection with a chunk on both shards.
        const onBothNs = `${dbName}.onBoth`;
        assert.commandWorked(db.adminCommand({shardCollection: onBothNs, key: {x: 1}}));
        assert.commandWorked(db.adminCommand({split: onBothNs, middle: {x: 0}}));
        assert.commandWorked(
            db.adminCommand({
                moveChunk: onBothNs,
                find: {x: 0},
                to: destShard.shardName,
                _waitForDelete: true,
            }),
        );

        // An unsplittable collection on the donor.
        const trackedNs = `${dbName}.tracked`;
        assert.commandWorked(
            db.runCommand({
                createUnsplittableCollection: "tracked",
                dataShard: donorShard.shardName,
            }),
        );

        const fp = configureFailPoint(
            donorShard,
            "hangAfterEnterInShardRoleCloneAuthoritativeMetadataDDL",
            {
                dbName: dbName,
            },
        );
        const awaitUpgrade = startParallelShell(() => {
            assert.commandWorked(
                db.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
            );
        }, mongos.port);

        fp.wait();
        assert.commandWorked(db.adminCommand({movePrimary: dbName, to: destShard.shardName}));
        fp.off();
        awaitUpgrade();

        st.awaitReplicationOnShards();

        for (const ns of [onDonorNs, movedNs, onBothNs, trackedNs]) {
            assertShardCatalogMatchesGlobal(destShard, ns, {isDbPrimary: true});
            assertShardCatalogMatchesGlobal(donorShard, ns, {isDbPrimary: false});
        }
    }

    // Test both shard0 -> shard1 and shard1 -> shard0 interleavings to ensure that we test the case
    // where movePrimary has to clone the collection metadata and no later cloning fixes it up.
    // I.e. the test may spuriously pass if the only interleaving we tested were:
    // * cloneAuthoritativeMetadata(db, shard0)
    // * movePrimary(db, from: shard0, to: shard1)
    // * cloneAuthoritativeMetadata(db, shard1)
    it("makes collection metadata authoritative when movePrimary races the cloning DDL (shard0 -> shard1)", function () {
        runMovePrimaryRaceDuringUpgrade(shard0, shard1);
    });

    it("makes collection metadata authoritative when movePrimary races the cloning DDL (shard1 -> shard0)", function () {
        runMovePrimaryRaceDuringUpgrade(shard1, shard0);
    });
});

// Fresh cluster: the authoritative collections are created by addShard's lastLTS->latest FCV upgrade.
describeOrSkip("authoritative shard catalog on a new latestFCV cluster", function () {
    it("exists on the shard", function () {
        const st = new ShardingTest({shards: 1, rs: {nodes: 1}});
        try {
            assertAuthoritativeShardCatalog(st.shard0, {
                authoritativeCollections: true,
                haveDatabases: false,
            });
        } finally {
            st.stop();
        }
    });
});
