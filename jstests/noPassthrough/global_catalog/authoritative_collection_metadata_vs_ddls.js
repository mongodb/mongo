/*
 * Validates that DDLs which update collection metadata also update the shard catalog
 * (config.shard.catalog.collections and config.shard.catalog.chunks) and that
 * checkMetadataConsistency reports no inconsistencies after each DDL.
 *
 * @tags: [
 *   featureFlagShardAuthoritativeCollMetadata,
 * ]
 */

import {after, afterEach, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("Authoritative collection metadata vs DDLs", function () {
    const kShardCatalogCollNs = "shard.catalog.collections";
    const kShardCatalogChunksNs = "shard.catalog.chunks";

    let st;
    let testCounter = 0;

    function uniqueDbName(suffix) {
        return `auth_coll_meta_${suffix}_${testCounter++}`;
    }

    function getGlobalCatalogCollMetadata(ns) {
        return st.s.getDB("config").collections.findOne({_id: ns});
    }

    function getGlobalCatalogChunks(uuid, shardId) {
        const query = {uuid: uuid, shard: shardId};
        return st.s.getDB("config").chunks.find(query).sort({min: 1}).toArray();
    }

    function getShardCatalogCollMetadata(node, ns) {
        return node.getDB("config").getCollection(kShardCatalogCollNs).findOne({_id: ns});
    }

    function getShardCatalogChunks(node, uuid) {
        return node.getDB("config").getCollection(kShardCatalogChunksNs).find({uuid: uuid}).sort({min: 1}).toArray();
    }

    function getAllGlobalCatalogChunks(uuid) {
        return st.s.getDB("config").chunks.find({uuid: uuid}).sort({min: 1}).toArray();
    }

    function assertShardCatalogOnNode(node, ns, {expectedUuid, expectedKey, expectedChunks}) {
        const label = node.host;
        const meta = getShardCatalogCollMetadata(node, ns);
        assert.neq(null, meta, `${label}: missing collection metadata in shard catalog`);
        assert.eq(expectedUuid.toString(), meta.uuid.toString(), `${label}: uuid mismatch`);
        assert.eq(tojson(expectedKey), tojson(meta.key), `${label}: shard key mismatch`);

        const shardChunks = getShardCatalogChunks(node, expectedUuid);
        assert.eq(expectedChunks.length, shardChunks.length, `${label}: chunk count mismatch`);

        for (let i = 0; i < expectedChunks.length; i++) {
            assert.eq(
                tojson(expectedChunks[i].min),
                tojson(shardChunks[i].min),
                `${label}: chunk ${i} min boundary mismatch`,
            );
            assert.eq(
                tojson(expectedChunks[i].max),
                tojson(shardChunks[i].max),
                `${label}: chunk ${i} max boundary mismatch`,
            );
        }
    }

    function getInMemoryCollectionMetadata(node, ns) {
        return assert.commandWorked(node.adminCommand({getShardVersion: ns, fullMetadata: true}));
    }

    before(function () {
        st = new ShardingTest({shards: 2, rs: {nodes: 3}});
    });

    after(function () {
        st.stop();
    });

    afterEach(function () {
        const inconsistencies = st.s.getDB("admin").checkMetadataConsistency().toArray();
        assert.eq(0, inconsistencies.length, `Metadata inconsistencies found: ${tojson(inconsistencies)}`);
    });

    describe("refineCollectionShardKey", function () {
        const dbName = uniqueDbName("refine");
        const ns = `${dbName}.coll`;

        it("updates shard catalog on both shards with refined key and correct chunk boundaries", function () {
            const db = st.s.getDB(dbName);
            assert.commandWorked(db.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
            assert.commandWorked(db.adminCommand({shardCollection: ns, key: {x: 1}}));

            assert.commandWorked(db.adminCommand({split: ns, middle: {x: 0}}));
            assert.commandWorked(
                db.adminCommand({moveChunk: ns, find: {x: 0}, to: st.shard1.shardName, _waitForDelete: true}),
            );
            assert.commandWorked(
                db.coll.insert([
                    {x: -1, y: "a"},
                    {x: 1, y: "b"},
                    {x: 5, y: "c"},
                ]),
            );

            assert.commandWorked(db.coll.createIndex({x: 1, y: 1}));
            assert.commandWorked(db.adminCommand({refineCollectionShardKey: ns, key: {x: 1, y: 1}}));

            const refinedKey = {x: 1, y: 1};
            const globalMeta = getGlobalCatalogCollMetadata(ns);
            assert.neq(null, globalMeta);
            const allGlobalChunks = getAllGlobalCatalogChunks(globalMeta.uuid);
            assert.eq(allGlobalChunks.length, 2);

            st.awaitReplicationOnShards();

            // TODO (SERVER-121707): Re-factor the following checks once refineCollectionShardKey
            // only fetches owned chunks.

            [
                {rs: st.rs0, shardName: st.shard0.shardName},
                {rs: st.rs1, shardName: st.shard1.shardName},
            ].forEach(({rs, shardName}) => {
                const shardGlobalChunks = getGlobalCatalogChunks(globalMeta.uuid, shardName);
                assert.gt(shardGlobalChunks.length, 0, `Expected at least one chunk on ${shardName}`);

                // Validate in-memory metadata on each shard's primary.
                const primaryNode = rs.getPrimary();
                const inMemoryMeta = getInMemoryCollectionMetadata(primaryNode, ns);
                assert.eq(
                    tojson(refinedKey),
                    tojson(inMemoryMeta.metadata.keyPattern),
                    `${shardName}: in-memory shard key mismatch`,
                );

                // Validate durable shard catalog on every node (primary + secondaries).
                rs.nodes.forEach((node) => {
                    assertShardCatalogOnNode(node, ns, {
                        expectedUuid: globalMeta.uuid,
                        expectedKey: refinedKey,
                        expectedChunks: allGlobalChunks,
                    });
                });
            });
        });

        it("cleans up stale shard catalog chunks when merge/split leaves leftovers", function () {
            const dbName2 = uniqueDbName("refine_stale");
            const ns2 = `${dbName2}.coll`;
            const db2 = st.s.getDB(dbName2);

            // Setup: sharded collection {x: 1} with 2 chunks split at x: 0, all on shard0.
            assert.commandWorked(db2.adminCommand({enableSharding: dbName2, primaryShard: st.shard0.shardName}));
            assert.commandWorked(db2.adminCommand({shardCollection: ns2, key: {x: 1}}));
            assert.commandWorked(db2.adminCommand({split: ns2, middle: {x: 0}}));
            assert.commandWorked(
                db2.coll.insert([
                    {x: -10, y: 1, z: 1},
                    {x: 10, y: 2, z: 2},
                ]),
            );

            // First refine: {x: 1} -> {x: 1, y: 1}.
            assert.commandWorked(db2.coll.createIndex({x: 1, y: 1}));
            assert.commandWorked(db2.adminCommand({refineCollectionShardKey: ns2, key: {x: 1, y: 1}}));

            st.awaitReplicationOnShards();

            // TODO (SERVER-121707): Re-factor the following checks once refineCollectionShardKey
            // only fetches owned chunks.

            // Verify shard catalog == global catalog after first refine (2 chunks, key {x, y}).
            {
                const globalMeta = getGlobalCatalogCollMetadata(ns2);
                const allGlobalChunks = getAllGlobalCatalogChunks(globalMeta.uuid);
                assert.eq(2, allGlobalChunks.length, "Expected 2 chunks after first refine");

                st.rs0.nodes.forEach((node) => {
                    assertShardCatalogOnNode(node, ns2, {
                        expectedUuid: globalMeta.uuid,
                        expectedKey: {x: 1, y: 1},
                        expectedChunks: allGlobalChunks,
                    });
                });
            }

            // Merge the 2 chunks into 1. Global catalog now has 1 chunk, but shard catalog is
            // stale: it still has 2 chunks with the old refined key.
            assert.commandWorked(
                db2.adminCommand({
                    mergeChunks: ns2,
                    bounds: [
                        {x: MinKey, y: MinKey},
                        {x: MaxKey, y: MaxKey},
                    ],
                }),
            );

            {
                const globalMeta = getGlobalCatalogCollMetadata(ns2);
                const allGlobalChunks = getAllGlobalCatalogChunks(globalMeta.uuid);
                assert.eq(1, allGlobalChunks.length, "Expected 1 chunk after merge");

                const shardChunks = getShardCatalogChunks(st.rs0.getPrimary(), globalMeta.uuid);
                assert.eq(2, shardChunks.length, "Shard catalog should still have 2 stale chunks");
            }

            // Second refine: {x: 1, y: 1} -> {x: 1, y: 1, z: 1}.
            assert.commandWorked(db2.coll.createIndex({x: 1, y: 1, z: 1}));
            assert.commandWorked(db2.adminCommand({refineCollectionShardKey: ns2, key: {x: 1, y: 1, z: 1}}));

            st.awaitReplicationOnShards();

            // Verify shard catalog has no leftover chunks and matches global catalog exactly.
            {
                const globalMeta = getGlobalCatalogCollMetadata(ns2);
                const allGlobalChunks = getAllGlobalCatalogChunks(globalMeta.uuid);
                assert.eq(1, allGlobalChunks.length, "Expected 1 chunk after second refine");

                st.rs0.nodes.forEach((node) => {
                    assertShardCatalogOnNode(node, ns2, {
                        expectedUuid: globalMeta.uuid,
                        expectedKey: {x: 1, y: 1, z: 1},
                        expectedChunks: allGlobalChunks,
                    });
                });
            }
        });
    });
});
