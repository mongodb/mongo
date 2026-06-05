/**
 * Validates that FCV upgrade/downgrade of the authoritative shard catalog works for both database
 * and collection metadata, and does not block setFCV.
 *
 * TODO (SERVER-98118): Remove this test.
 *
 * @tags: [
 *   featureFlagAuthoritativeShardsCRUD,
 *   featureFlagAuthoritativeShardsDDL,
 * ]
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {afterEach, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const describeOrSkip = (() => {
    let st;
    try {
        st = new ShardingTest({shards: 1, mongos: 1, rs: {nodes: 1}});
        assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));

        const db = st.s.getDB("admin");
        if (
            FeatureFlagUtil.isPresentAndEnabled(db, "AuthoritativeShardsDDL") ||
            FeatureFlagUtil.isPresentAndEnabled(db, "AuthoritativeShardsCRUD")
        ) {
            jsTest.log.info("Skipping test because AuthoritativeShards feature flags are already enabled in lastLTS");
            return describe.skip;
        }
    } finally {
        if (st) {
            st.stop();
        }
    }

    return describe;
})();

describeOrSkip("FCV lifecycle for authoritative metadata", function () {
    const kDbName = "testDb";
    const kAuthoritativeDb = "config";
    const kAuthoritativeColl = "shard.catalog.databases";
    const kShardCatalogColls = "shard.catalog.collections";
    const kShardCatalogChunks = "shard.catalog.chunks";

    let st, mongos, shard0, shard1;

    function assertAuthoritativeCollectionExists(shard, {shouldExist}) {
        const colls = shard.getDB(kAuthoritativeDb).getCollectionNames();
        if (shouldExist) {
            assert.contains(kAuthoritativeColl, colls, `Authoritative collection should exist on ${shard.shardName}`);
        } else {
            assert.eq(
                -1,
                colls.indexOf(kAuthoritativeColl),
                `Authoritative collection should not exist on ${shard.shardName}`,
            );
        }
    }

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

    beforeEach(function () {
        st = new ShardingTest({shards: 2, mongos: 1, rs: {nodes: 1}});
        mongos = st.s;
        shard0 = st.shard0;
        shard1 = st.shard1;

        assert.commandWorked(mongos.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));

        assert.commandWorked(mongos.adminCommand({enableSharding: kDbName, primaryShard: st.shard0.shardName}));
    });

    afterEach(function () {
        if (st) {
            st.stop();
        }
    });

    it("should correctly create and remove metadata on FCV upgrade and downgrade", function () {
        assertFeatureFlags({enabled: false});
        assertAuthoritativeCollectionExists(shard0, {shouldExist: false});

        // The upgrade should succeed, creating the collection on shard0.
        assert.commandWorked(mongos.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));

        assertFeatureFlags({enabled: true});
        assertAuthoritativeCollectionExists(shard0, {shouldExist: true});
        assertAuthoritativeCollectionExists(shard1, {shouldExist: false});

        // The downgrade should succeed, dropping the collection from shard0.
        assert.commandWorked(mongos.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));

        assertAuthoritativeCollectionExists(shard0, {shouldExist: false});
    });

    it("should drop pre-existing metadata collection on FCV upgrade", function () {
        assertFeatureFlags({enabled: false});
        assertAuthoritativeCollectionExists(shard0, {shouldExist: false});

        assert.commandWorked(
            shard0.getDB(kAuthoritativeDb).getCollection(kAuthoritativeColl).insertOne({
                _id: kDbName,
            }),
        );
        st.rs0.awaitLastOpCommitted();

        // Manually create the collection on shard0 to simulate a dirty state.
        assertAuthoritativeCollectionExists(shard0, {shouldExist: true});

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
        assertAuthoritativeCollectionExists(shard0, {shouldExist: true});
        assertAuthoritativeCollectionExists(shard1, {shouldExist: false});
    });

    it("should drop pre-existing metadata collection on a non-primary shard during FCV upgrade", function () {
        assertFeatureFlags({enabled: false});
        assertAuthoritativeCollectionExists(shard1, {shouldExist: false});

        // Manually create the collection on the a non-primary shard.
        assert.commandWorked(
            shard1.getDB(kAuthoritativeDb).getCollection(kAuthoritativeColl).insertOne({
                _id: kDbName,
            }),
        );
        st.rs1.awaitLastOpCommitted();
        assertAuthoritativeCollectionExists(shard1, {shouldExist: true});

        // The upgrade should succeed, dropping the bad collection on shard1 and creating the
        // correct one on shard0.
        assert.commandWorked(
            mongos.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
            "Actual upgrade should have succeeded",
        );

        assertFeatureFlags({enabled: true});
        assertAuthoritativeCollectionExists(shard0, {shouldExist: true});
        assertAuthoritativeCollectionExists(shard1, {shouldExist: false});
    });

    it("should drop metadata collection from all shards on FCV downgrade", function () {
        assert.commandWorked(mongos.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));
        assertAuthoritativeCollectionExists(shard0, {shouldExist: true});
        assertAuthoritativeCollectionExists(shard1, {shouldExist: false});

        // Manually create the collection on shard1 to simulate a dirty state.
        assert.commandWorked(
            shard1.getDB(kAuthoritativeDb).getCollection(kAuthoritativeColl).insertOne({
                _id: kDbName,
            }),
        );
        st.rs1.awaitLastOpCommitted();
        assertAuthoritativeCollectionExists(shard1, {shouldExist: true});

        assert.commandWorked(mongos.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));

        // Assert the collection is gone from both shards.
        assertFeatureFlags({enabled: false});
        assertAuthoritativeCollectionExists(shard0, {shouldExist: false});
        assertAuthoritativeCollectionExists(shard1, {shouldExist: false});
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
        const shardMeta = shard.getDB("config").getCollection(kShardCatalogColls).findOne({_id: ns});
        if (!hasEntry) {
            assert.eq(null, shardMeta, `${ns}: unexpected metadata on ${shard.shardName}`);
        } else {
            assert.neq(null, shardMeta, `${ns}: missing metadata on ${shard.shardName}`);
            assert.eq(meta.uuid.toString(), shardMeta.uuid.toString(), `${ns}: uuid mismatch on ${shard.shardName}`);
            assert.eq(tojson(meta.key), tojson(shardMeta.key), `${ns}: shard key mismatch on ${shard.shardName}`);
        }

        const shardChunks = shard
            .getDB("config")
            .getCollection(kShardCatalogChunks)
            .find({uuid: meta.uuid})
            .sort({min: 1})
            .toArray();
        assert.eq(ownedChunks.length, shardChunks.length, `${ns}: chunk count mismatch on ${shard.shardName}`);
        ownedChunks.forEach((chunk, i) => {
            assert.eq(tojson(chunk.min), tojson(shardChunks[i].min), `${ns}: chunk ${i} min mismatch`);
            assert.eq(tojson(chunk.max), tojson(shardChunks[i].max), `${ns}: chunk ${i} max mismatch`);
        });
    }

    it("should clone collection and chunk metadata to owning shards on FCV upgrade", function () {
        const db = mongos.getDB(kDbName);

        // A sharded collection with one chunk on each shard.
        const shardedNs = `${kDbName}.sharded`;
        assert.commandWorked(db.adminCommand({shardCollection: shardedNs, key: {x: 1}}));
        assert.commandWorked(db.adminCommand({split: shardedNs, middle: {x: 0}}));
        assert.commandWorked(
            db.adminCommand({moveChunk: shardedNs, find: {x: 0}, to: shard1.shardName, _waitForDelete: true}),
        );

        // A tracked unsplittable collection living on the non-primary shard.
        const trackedNs = `${kDbName}.tracked`;
        assert.commandWorked(db.runCommand({createUnsplittableCollection: "tracked", dataShard: shard1.shardName}));

        // Nothing is cloned into the shard catalogs while still on lastLTS.
        assert.eq(null, shard0.getDB("config").getCollection(kShardCatalogColls).findOne({_id: shardedNs}));
        assert.eq(null, shard1.getDB("config").getCollection(kShardCatalogColls).findOne({_id: shardedNs}));

        assert.commandWorked(mongos.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));
        st.awaitReplicationOnShards();

        assertShardCatalogMatchesGlobal(shard0, shardedNs, {isDbPrimary: true});
        assertShardCatalogMatchesGlobal(shard1, shardedNs, {isDbPrimary: false});

        assertShardCatalogMatchesGlobal(shard1, trackedNs, {isDbPrimary: false});
        assertShardCatalogMatchesGlobal(shard0, trackedNs, {isDbPrimary: true});
    });
});
