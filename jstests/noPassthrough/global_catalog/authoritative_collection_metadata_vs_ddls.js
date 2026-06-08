/*
 * Validates that DDLs which update collection metadata also update the shard catalog
 * (config.shard.catalog.collections and config.shard.catalog.chunks), that in-memory filtering
 * metadata on shard nodes is consistent after each DDL, and that checkMetadataConsistency reports
 * no inconsistencies after each DDL.
 *
 * @tags: [
 *   featureFlagAuthoritativeShardsDDL,
 *   requires_timeseries,
 * ]
 */

import {
    getTimeseriesBucketsColl,
    getTimeseriesCollForDDLOps,
} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
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
        const query = {uuid: uuid, $or: [{shard: shardId}, {"history.shard": shardId}]};
        return st.s.getDB("config").chunks.find(query).sort({min: 1}).toArray();
    }

    function getAllGlobalCatalogChunks(uuid) {
        return st.s.getDB("config").chunks.find({uuid: uuid}).sort({min: 1}).toArray();
    }

    function getShardCatalogCollMetadata(node, ns) {
        return node.getDB("config").getCollection(kShardCatalogCollNs).findOne({_id: ns});
    }

    function getShardCatalogChunks(node, uuid) {
        return node.getDB("config").getCollection(kShardCatalogChunksNs).find({uuid: uuid}).sort({min: 1}).toArray();
    }

    function getInMemoryCollectionMetadata(node, ns) {
        return assert.commandWorked(node.adminCommand({getShardVersion: ns, fullMetadata: true}));
    }

    function forEachNodeOnAllShards(fn) {
        [st.rs0, st.rs1].forEach((rs) => rs.nodes.forEach(fn));
    }

    function assertShardCatalogOnNode(
        node,
        ns,
        {expectedUuid, expectedKey, expectedChunks, expectedTimeseriesFields, expectedTimestamp},
    ) {
        const label = node.host;
        const meta = getShardCatalogCollMetadata(node, ns);
        assert.neq(null, meta, `${label}: missing collection metadata in shard catalog`);
        assert.eq(expectedUuid.toString(), meta.uuid.toString(), `${label}: uuid mismatch`);
        assert.eq(tojson(expectedKey), tojson(meta.key), `${label}: shard key mismatch`);
        if (expectedTimestamp) {
            assert.eq(tojson(expectedTimestamp), tojson(meta.timestamp), `${label}: timestamp mismatch`);
        }
        if (expectedTimeseriesFields) {
            assert.eq(
                tojson(expectedTimeseriesFields),
                tojson(meta.timeseriesFields),
                `${label}: time-series fields mismatch`,
            );
        }

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

    function assertShardCatalogAbsentOnNode(node, ns, uuid) {
        const label = node.host;
        assertShardCatalogCollMetadataAbsentAtNsOnNode(node, ns);
        assert.eq(0, getShardCatalogChunks(node, uuid).length, `${label}: unexpected chunks in shard catalog`);
    }

    function assertShardCatalogCollMetadataAbsentAtNsOnNode(node, ns) {
        const label = node.host;
        assert.eq(
            null,
            getShardCatalogCollMetadata(node, ns),
            `${label}: unexpected collection metadata in shard catalog`,
        );
    }

    function assertShardCatalogChunklessOnNode(
        node,
        ns,
        {expectedUuid, expectedKey, expectedTimeseriesFields, expectedTimestamp},
    ) {
        const label = node.host;
        const meta = getShardCatalogCollMetadata(node, ns);
        assert.neq(null, meta, `${label}: chunkless primary is missing collection metadata`);
        assert.eq(expectedUuid.toString(), meta.uuid.toString(), `${label}: uuid mismatch`);
        assert.eq(tojson(expectedKey), tojson(meta.key), `${label}: shard key mismatch`);
        if (expectedTimestamp) {
            assert.eq(tojson(expectedTimestamp), tojson(meta.timestamp), `${label}: timestamp mismatch`);
        }
        if (expectedTimeseriesFields) {
            assert.eq(
                tojson(expectedTimeseriesFields),
                tojson(meta.timeseriesFields),
                `${label}: time-series fields mismatch`,
            );
        }

        const shardChunks = getShardCatalogChunks(node, expectedUuid);
        assert.eq(0, shardChunks.length, `${label}: chunkless primary must not carry shard catalog chunks`);
    }

    // We assert the in-memory metadata is not sharded rather than completely absent because a
    // non-existent collection is considered untracked by the server, so it is valid for the CSS
    // to retain an UNTRACKED token after the drop. The important invariant is that the CSS does
    // not report the collection as sharded (no shard key pattern).
    function assertInMemoryMetadataNotSharded(node, ns) {
        const label = node.host;
        const res = getInMemoryCollectionMetadata(node, ns);
        if (res.metadata) {
            assert.eq(
                undefined,
                res.metadata.keyPattern,
                `${label}: in-memory metadata still shows collection as sharded after drop`,
            );
        }
    }

    function assertInMemoryMetadataSharded(node, ns, expectedKey) {
        const label = node.host;
        const res = getInMemoryCollectionMetadata(node, ns);
        assert(res.metadata, `${label}: expected in-memory metadata to be present`);
        assert.neq(res.metadata, {}, `${label}: expected in-memory metadata to be present`);
        assert.eq(tojson(expectedKey), tojson(res.metadata.keyPattern), `${label}: in-memory shard key mismatch`);
    }

    function dropCollectionAndAssertCleanup(db, collName) {
        const ns = `${db.getName()}.${collName}`;

        const globalMeta = getGlobalCatalogCollMetadata(ns);
        assert.neq(null, globalMeta, `${ns}: expected in global catalog before drop`);
        const uuid = globalMeta.uuid;

        assert.commandWorked(db.runCommand({drop: collName}));

        assert.eq(null, getGlobalCatalogCollMetadata(ns), `${ns}: still in global catalog after drop`);
        assert.eq(0, getAllGlobalCatalogChunks(uuid).length, `${ns}: chunks still in global catalog after drop`);

        st.awaitReplicationOnShards();
        forEachNodeOnAllShards((node) => {
            assertShardCatalogAbsentOnNode(node, ns, uuid);
            assertInMemoryMetadataNotSharded(node, ns);
        });
    }

    function setupDb(suffix) {
        const dbName = uniqueDbName(suffix);
        const db = st.s.getDB(dbName);
        assert.commandWorked(db.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
        return db;
    }

    function assertTrackedCollectionMetadataOnShardCatalogs(
        ns,
        {oldTrackedNs, dbPrimaryShardName = st.shard0.shardName},
    ) {
        const globalMeta = getGlobalCatalogCollMetadata(ns);
        assert.neq(null, globalMeta, `${ns}: missing collection metadata in global catalog`);

        [
            {rs: st.rs0, shardName: st.shard0.shardName},
            {rs: st.rs1, shardName: st.shard1.shardName},
        ].forEach(({rs, shardName}) => {
            const shardGlobalChunks = getGlobalCatalogChunks(globalMeta.uuid, shardName);

            rs.nodes.forEach((node) => {
                assertShardCatalogCollMetadataAbsentAtNsOnNode(node, oldTrackedNs);

                if (shardGlobalChunks.length > 0) {
                    assertShardCatalogOnNode(node, ns, {
                        expectedUuid: globalMeta.uuid,
                        expectedKey: globalMeta.key,
                        expectedChunks: shardGlobalChunks,
                        expectedTimeseriesFields: globalMeta.timeseriesFields,
                        expectedTimestamp: globalMeta.timestamp,
                    });
                } else if (shardName === dbPrimaryShardName) {
                    assertShardCatalogChunklessOnNode(node, ns, {
                        expectedUuid: globalMeta.uuid,
                        expectedKey: globalMeta.key,
                        expectedTimeseriesFields: globalMeta.timeseriesFields,
                        expectedTimestamp: globalMeta.timestamp,
                    });
                } else {
                    assertShardCatalogAbsentOnNode(node, ns, globalMeta.uuid);
                }
            });
        });
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
        it("updates shard catalog on both shards with refined key and correct chunk boundaries", function () {
            const db = setupDb("refine");
            const ns = `${db.getName()}.coll`;

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

            [
                {rs: st.rs0, shardName: st.shard0.shardName},
                {rs: st.rs1, shardName: st.shard1.shardName},
            ].forEach(({rs, shardName}) => {
                const shardGlobalChunks = getGlobalCatalogChunks(globalMeta.uuid, shardName);
                assert.gt(shardGlobalChunks.length, 0, `Expected at least one chunk on ${shardName}`);

                // Validate in-memory metadata on each shard's primary.
                assertInMemoryMetadataSharded(rs.getPrimary(), ns, refinedKey);

                // Validate durable shard catalog on every node (primary + secondaries).
                rs.nodes.forEach((node) => {
                    assertShardCatalogOnNode(node, ns, {
                        expectedUuid: globalMeta.uuid,
                        expectedKey: refinedKey,
                        expectedChunks: shardGlobalChunks,
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

    describe("movePrimary", function () {
        function assertCollectionExistsWithoutChunksOnNode(node, ns, expectedUuid) {
            const label = node.host;
            const meta = getShardCatalogCollMetadata(node, ns);
            assert.neq(null, meta);
            assert.eq(expectedUuid.toString(), meta.uuid.toString());

            const shardChunks = getShardCatalogChunks(node, expectedUuid);
            assert.eq(0, shardChunks.length);
        }

        it("commits collection metadata on new primary for tracked collection with no chunks there", function () {
            const db = setupDb("moveprimary_chunkless");
            const dbName = db.getName();
            const ns = `${dbName}.coll`;

            assert.commandWorked(db.adminCommand({shardCollection: ns, key: {x: 1}}));
            assert.commandWorked(db.coll.insert([{x: 1}]));

            const globalMeta = getGlobalCatalogCollMetadata(ns);

            st.awaitReplicationOnShards();
            st.rs1.nodes.forEach((node) => assertShardCatalogAbsentOnNode(node, ns, globalMeta.uuid));

            assert.commandWorked(db.adminCommand({movePrimary: dbName, to: st.shard1.shardName}));
            st.awaitReplicationOnShards();

            st.rs1.nodes.forEach((node) => assertCollectionExistsWithoutChunksOnNode(node, ns, globalMeta.uuid));
        });

        it("does not commit collection metadata when new primary already owns chunks", function () {
            const db = setupDb("moveprimary_has_chunks");
            const dbName = db.getName();
            const ns = `${dbName}.coll`;

            assert.commandWorked(db.adminCommand({shardCollection: ns, key: {x: 1}}));
            assert.commandWorked(db.adminCommand({split: ns, middle: {x: 0}}));
            assert.commandWorked(
                db.adminCommand({moveChunk: ns, find: {x: 0}, to: st.shard1.shardName, _waitForDelete: true}),
            );
            assert.commandWorked(db.coll.insert([{x: -1}, {x: 1}]));

            const globalMeta = getGlobalCatalogCollMetadata(ns);

            assert.commandWorked(db.adminCommand({movePrimary: dbName, to: st.shard1.shardName}));
            st.awaitReplicationOnShards();

            const shard1GlobalChunks = getGlobalCatalogChunks(globalMeta.uuid, st.shard1.shardName);
            assert.gt(shard1GlobalChunks.length, 0);
        });
    });

    describe("collMod", function () {
        it("updates shard catalog time-series fields and chunk versions", function () {
            const db = setupDb("collmod_ts");
            const collName = "ts";
            const coll = db.getCollection(collName);
            const ns = coll.getFullName();

            assert.commandWorked(
                db.createCollection(collName, {
                    timeseries: {timeField: "time", metaField: "tag", granularity: "seconds"},
                }),
            );
            assert.commandWorked(db.adminCommand({shardCollection: ns, key: {tag: 1}}));

            const ddlNs = getTimeseriesCollForDDLOps(db, coll).getFullName();
            assert.commandWorked(db.adminCommand({split: ddlNs, middle: {meta: 0}}));
            assert.commandWorked(
                db.adminCommand({moveChunk: ddlNs, find: {meta: 1}, to: st.shard1.shardName, _waitForDelete: true}),
            );
            assert.commandWorked(
                coll.insert([
                    {time: ISODate("2026-01-01T00:00:00.000Z"), tag: -1},
                    {time: ISODate("2026-01-01T00:00:00.000Z"), tag: 1},
                ]),
            );

            const originalGlobalMeta = getGlobalCatalogCollMetadata(ddlNs);
            const originalGlobalChunks = getAllGlobalCatalogChunks(originalGlobalMeta.uuid);
            assert.eq(2, originalGlobalChunks.length, `${ddlNs}: expected two chunks before collMod`);

            assert.commandWorked(db.runCommand({collMod: collName, timeseries: {granularity: "minutes"}}));

            const globalMeta = getGlobalCatalogCollMetadata(ddlNs);
            assert.neq(null, globalMeta, `${ddlNs}: missing global catalog metadata after collMod`);
            assert.eq("minutes", globalMeta.timeseriesFields.granularity);

            const allGlobalChunks = getAllGlobalCatalogChunks(globalMeta.uuid);
            assert.eq(2, allGlobalChunks.length, `${ddlNs}: expected two chunks after collMod`);
            for (let i = 0; i < allGlobalChunks.length; i++) {
                assert.neq(
                    tojson(originalGlobalChunks[i].lastmod),
                    tojson(allGlobalChunks[i].lastmod),
                    `${ddlNs}: expected chunk ${i} version to change after collMod`,
                );
            }

            st.awaitReplicationOnShards();

            [
                {rs: st.rs0, shardName: st.shard0.shardName},
                {rs: st.rs1, shardName: st.shard1.shardName},
            ].forEach(({rs, shardName}) => {
                const shardGlobalChunks = getGlobalCatalogChunks(globalMeta.uuid, shardName);
                assert.gt(shardGlobalChunks.length, 0, `Expected at least one chunk on ${shardName}`);

                assertInMemoryMetadataSharded(rs.getPrimary(), ddlNs, globalMeta.key);

                rs.nodes.forEach((node) => {
                    assertShardCatalogOnNode(node, ddlNs, {
                        expectedUuid: globalMeta.uuid,
                        expectedKey: globalMeta.key,
                        expectedChunks: shardGlobalChunks,
                        expectedTimeseriesFields: globalMeta.timeseriesFields,
                    });
                });
            });
        });
    });

    describe("dropCollection", function () {
        it("cleans up shard catalog on all nodes for a multi-shard sharded collection", function () {
            const db = setupDb("drop_multi");
            const ns = `${db.getName()}.coll`;

            assert.commandWorked(db.adminCommand({shardCollection: ns, key: {x: 1}}));
            assert.commandWorked(db.adminCommand({split: ns, middle: {x: 0}}));
            assert.commandWorked(
                db.adminCommand({moveChunk: ns, find: {x: 0}, to: st.shard1.shardName, _waitForDelete: true}),
            );
            assert.commandWorked(db.coll.insert([{x: -1}, {x: 1}]));

            dropCollectionAndAssertCleanup(db, "coll");
        });

        it("cleans up shard catalog for a single-shard sharded collection", function () {
            const db = setupDb("drop_single");
            const ns = `${db.getName()}.coll`;

            assert.commandWorked(db.adminCommand({shardCollection: ns, key: {x: 1}}));
            assert.commandWorked(db.coll.insert([{x: 1}, {x: 2}]));

            dropCollectionAndAssertCleanup(db, "coll");
        });

        it("cleans up shard catalog for an unsharded tracked collection after moveCollection", function () {
            const db = setupDb("drop_tracked");
            const ns = `${db.getName()}.coll`;

            assert.commandWorked(db.coll.insert([{x: 1}, {x: 2}]));
            assert.commandWorked(db.adminCommand({moveCollection: ns, toShard: st.shard1.shardName}));

            dropCollectionAndAssertCleanup(db, "coll");
        });
    });

    describe("untrackUnshardedCollection", function () {
        it("cleans up shard catalog and preserves data for a tracked unsplittable collection", function () {
            const db = setupDb("untrack");
            const ns = `${db.getName()}.coll`;

            assert.commandWorked(db.createCollection("coll"));
            assert.commandWorked(db.adminCommand({moveCollection: ns, toShard: st.shard1.shardName}));
            assert.commandWorked(db.adminCommand({moveCollection: ns, toShard: st.shard0.shardName}));
            assert.commandWorked(db.coll.insert([{x: 1}, {x: 2}]));

            const globalMeta = getGlobalCatalogCollMetadata(ns);
            assert.neq(null, globalMeta, `${ns}: expected in global catalog before untrack`);
            const uuid = globalMeta.uuid;

            assert.commandWorked(db.adminCommand({untrackUnshardedCollection: ns}));

            assert.eq(null, getGlobalCatalogCollMetadata(ns), `${ns}: still in global catalog after untrack`);
            assert.eq(0, getAllGlobalCatalogChunks(uuid).length, `${ns}: chunks still in global catalog after untrack`);
            assert.eq(2, db.coll.countDocuments({}), `${ns}: user data should remain after untrack`);

            st.awaitReplicationOnShards();
            forEachNodeOnAllShards((node) => {
                assertShardCatalogAbsentOnNode(node, ns, uuid);
                assertInMemoryMetadataNotSharded(node, ns);
            });
        });
    });

    describe("dropDatabase", function () {
        it("cleans up shard catalog for all tracked collections in the database", function () {
            const db = setupDb("dropdb");
            const dbName = db.getName();
            const ns1 = `${dbName}.coll1`;
            const ns2 = `${dbName}.coll2`;

            assert.commandWorked(db.adminCommand({shardCollection: ns1, key: {a: 1}}));
            assert.commandWorked(db.adminCommand({shardCollection: ns2, key: {b: 1}}));
            assert.commandWorked(db.coll1.insert([{a: 1}]));
            assert.commandWorked(db.coll2.insert([{b: 1}]));

            const uuid1 = getGlobalCatalogCollMetadata(ns1).uuid;
            const uuid2 = getGlobalCatalogCollMetadata(ns2).uuid;

            assert.commandWorked(db.dropDatabase());

            st.awaitReplicationOnShards();

            forEachNodeOnAllShards((node) => {
                assertShardCatalogAbsentOnNode(node, ns1, uuid1);
                assertShardCatalogAbsentOnNode(node, ns2, uuid2);
                assertInMemoryMetadataNotSharded(node, ns1);
                assertInMemoryMetadataNotSharded(node, ns2);
            });
        });
    });

    describe("convertToCapped", function () {
        // For a convertToCapped on a tracked unsplittable collection: no node must retain chunks
        // for the original UUID, the data shard must hold the new UUID's entry with real chunks,
        // the DB primary must hold the new UUID's entry, and any other shard must have no entry for the collection.
        function assertShardCatalogAfterConvertToCapped(
            ns,
            {dataShardRs, primaryRs, newUuid, originalUuid, unsplittableKey},
        ) {
            // The old UUID must be gone from every shard's chunk catalog.
            forEachNodeOnAllShards((node) => {
                assert.eq(
                    0,
                    getShardCatalogChunks(node, originalUuid).length,
                    `${node.host}: stale chunks for original uuid still present`,
                );
            });

            const newGlobalChunks = getAllGlobalCatalogChunks(newUuid);
            assert.eq(1, newGlobalChunks.length, `Expected 1 chunk after convertToCapped on ${ns}`);

            // Data shard must carry the new UUID with the authoritative chunks.
            dataShardRs.nodes.forEach((node) => {
                assertShardCatalogOnNode(node, ns, {
                    expectedUuid: newUuid,
                    expectedKey: unsplittableKey,
                    expectedChunks: newGlobalChunks,
                });
            });

            if (primaryRs !== dataShardRs) {
                // Primary is not the data shard: it owns no real chunks but must carry the collection entry so disk recovery recognize the collection as tracked.
                primaryRs.nodes.forEach((node) => {
                    const meta = getShardCatalogCollMetadata(node, ns);
                    assert.neq(null, meta, `${node.host}: chunkless primary is missing collection metadata`);
                    assert.eq(
                        newUuid.toString(),
                        meta.uuid.toString(),
                        `${node.host}: chunkless primary still has old uuid`,
                    );

                    const shardChunks = getShardCatalogChunks(node, newUuid);
                    assert.eq(0, shardChunks.length, `${node.host}: chunkless primary must not have any chunks`);
                });
            }

            // Any shard that is neither the data shard nor the DB primary must have no entry at
            // all for the collection.
            [st.rs0, st.rs1].forEach((rs) => {
                if (rs === dataShardRs || rs === primaryRs) {
                    return;
                }
                rs.nodes.forEach((node) => {
                    assertShardCatalogAbsentOnNode(node, ns, newUuid);
                });
            });
        }

        const kUnsplittableShardKey = {_id: 1};

        it("updates shard catalog when data shard is the DB primary", function () {
            const db = setupDb("capped_primary");
            const ns = `${db.getName()}.coll`;

            // Tracked, unsplittable collection living on the DB primary.
            assert.commandWorked(db.coll.insert([{x: 1}, {x: 2}]));
            assert.commandWorked(db.adminCommand({moveCollection: ns, toShard: st.shard0.shardName}));

            const originalUuid = getGlobalCatalogCollMetadata(ns).uuid;

            assert.commandWorked(db.runCommand({convertToCapped: "coll", size: 1024}));

            const newMeta = getGlobalCatalogCollMetadata(ns);
            assert.neq(null, newMeta, `${ns}: missing in global catalog after convertToCapped`);
            assert.neq(
                originalUuid.toString(),
                newMeta.uuid.toString(),
                "convertToCapped must reissue the collection UUID",
            );

            st.awaitReplicationOnShards();

            assertShardCatalogAfterConvertToCapped(ns, {
                dataShardRs: st.rs0,
                primaryRs: st.rs0,
                newUuid: newMeta.uuid,
                originalUuid: originalUuid,
                unsplittableKey: kUnsplittableShardKey,
            });
        });

        it("updates shard catalog when data shard differs from the DB primary", function () {
            const db = setupDb("capped_nonprimary");
            const ns = `${db.getName()}.coll`;

            // Tracked, unsplittable collection moved to a shard that is not the DB primary.
            assert.commandWorked(db.coll.insert([{x: 1}, {x: 2}]));
            assert.commandWorked(db.adminCommand({moveCollection: ns, toShard: st.shard1.shardName}));

            const originalUuid = getGlobalCatalogCollMetadata(ns).uuid;

            assert.commandWorked(db.runCommand({convertToCapped: "coll", size: 1024}));

            const newMeta = getGlobalCatalogCollMetadata(ns);
            assert.neq(null, newMeta);
            assert.neq(originalUuid.toString(), newMeta.uuid.toString());

            st.awaitReplicationOnShards();

            assertShardCatalogAfterConvertToCapped(ns, {
                dataShardRs: st.rs1,
                primaryRs: st.rs0,
                newUuid: newMeta.uuid,
                originalUuid: originalUuid,
                unsplittableKey: kUnsplittableShardKey,
            });
        });

        it("subsequent drop cleans up shard catalog on all nodes", function () {
            // Guarantees convertToCapped leaves the shard catalog in a state from which a normal
            // drop can fully clean up, covering the drop-after-capped path end-to-end.
            const db = setupDb("capped_then_drop");
            const ns = `${db.getName()}.coll`;

            assert.commandWorked(db.coll.insert([{x: 1}]));
            assert.commandWorked(db.adminCommand({moveCollection: ns, toShard: st.shard1.shardName}));
            assert.commandWorked(db.runCommand({convertToCapped: "coll", size: 1024}));

            dropCollectionAndAssertCleanup(db, "coll");
        });
    });

    describe("viewless time-series upgrade/downgrade", function () {
        it("moves shard catalog metadata between user and bucket namespaces", function () {
            if (lastLTSFCV !== "8.0") {
                jsTest.log.info("Skipping test because last LTS FCV is no longer 8.0");
                return;
            }

            const db = setupDb("ts_upgrade_downgrade");

            const shardedCollName = "shardedTs";
            const shardedColl = db.getCollection(shardedCollName);
            const shardedUserNs = shardedColl.getFullName();
            const shardedBucketsNs = getTimeseriesBucketsColl(shardedColl).getFullName();

            assert.commandWorked(
                db.adminCommand({
                    shardCollection: shardedUserNs,
                    key: {tag: 1},
                    timeseries: {timeField: "time", metaField: "tag"},
                }),
            );
            assert.commandWorked(db.adminCommand({split: shardedUserNs, middle: {meta: 0}}));
            assert.commandWorked(
                db.adminCommand({
                    moveChunk: shardedUserNs,
                    find: {meta: 1},
                    to: st.shard1.shardName,
                    _waitForDelete: true,
                }),
            );
            assert.commandWorked(
                shardedColl.insert([
                    {time: ISODate("2026-01-01T00:00:00.000Z"), tag: -1},
                    {time: ISODate("2026-01-01T00:00:00.000Z"), tag: 1},
                ]),
            );

            const trackedCollName = "trackedTs";
            const trackedColl = db.getCollection(trackedCollName);
            const trackedUserNs = trackedColl.getFullName();
            const trackedBucketsNs = getTimeseriesBucketsColl(trackedColl).getFullName();

            assert.commandWorked(
                db.runCommand({
                    createUnsplittableCollection: trackedCollName,
                    dataShard: st.shard1.shardName,
                    timeseries: {timeField: "time", metaField: "tag"},
                }),
            );
            assert.commandWorked(trackedColl.insert([{time: ISODate("2026-01-01T00:00:00.000Z"), tag: "tracked"}]));

            assert.neq(null, getGlobalCatalogCollMetadata(shardedUserNs));
            assert.neq(null, getGlobalCatalogCollMetadata(trackedUserNs));

            assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));
            st.awaitReplicationOnShards();

            assert.eq(null, getGlobalCatalogCollMetadata(shardedUserNs));
            assert.eq(null, getGlobalCatalogCollMetadata(trackedUserNs));
            assert.neq(null, getGlobalCatalogCollMetadata(shardedBucketsNs));
            assert.neq(null, getGlobalCatalogCollMetadata(trackedBucketsNs));
            // Downgrade drops the shard catalog collections.
            forEachNodeOnAllShards((node) => {
                assertShardCatalogCollMetadataAbsentAtNsOnNode(node, shardedUserNs);
                assertShardCatalogCollMetadataAbsentAtNsOnNode(node, shardedBucketsNs);
                assertShardCatalogCollMetadataAbsentAtNsOnNode(node, trackedUserNs);
                assertShardCatalogCollMetadataAbsentAtNsOnNode(node, trackedBucketsNs);
            });

            assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));
            st.awaitReplicationOnShards();

            assert.eq(null, getGlobalCatalogCollMetadata(shardedBucketsNs));
            assert.eq(null, getGlobalCatalogCollMetadata(trackedBucketsNs));
            assertTrackedCollectionMetadataOnShardCatalogs(shardedUserNs, {
                oldTrackedNs: shardedBucketsNs,
            });
            assertTrackedCollectionMetadataOnShardCatalogs(trackedUserNs, {
                oldTrackedNs: trackedBucketsNs,
            });

            assert.eq(2, shardedColl.countDocuments({}));
            assert.eq(1, trackedColl.countDocuments({}));
        });
    });

    describe("renameCollection", () => {
        it("unsharded collection renamed to sharded collection namespace, replacing it", function () {
            const db = setupDb("rename_to_sharded");
            const srcNs = `${db.getName()}.src`;
            const dstNs = `${db.getName()}.dst`;

            assert.commandWorked(db.adminCommand({shardCollection: dstNs, key: {x: 1}}));
            assert.commandWorked(db.adminCommand({split: dstNs, middle: {x: 0}}));
            assert.commandWorked(
                db.adminCommand({moveChunk: dstNs, find: {x: 0}, to: st.shard1.shardName, _waitForDelete: true}),
            );
            assert.commandWorked(db.dst.insert([{x: -1}, {x: 1}]));
            assert.commandWorked(db.src.insert([{x: 10}, {x: 20}]));

            const originalDstUuid = getGlobalCatalogCollMetadata(dstNs).uuid;

            assert.commandWorked(db.adminCommand({renameCollection: srcNs, to: dstNs, dropTarget: true}));

            st.awaitReplicationOnShards();

            // No entries for the original sharded collection and no chunks for the final namespace.
            forEachNodeOnAllShards((node) => {
                assertShardCatalogAbsentOnNode(node, dstNs, originalDstUuid);
                assertInMemoryMetadataNotSharded(node, dstNs);
            });
        });

        it("unsharded collection renamed to unsharded collection without replacing it", function () {
            const db = setupDb("rename_unsharded_no_replace");
            const srcNs = `${db.getName()}.src`;
            const dstNs = `${db.getName()}.dst`;

            assert.commandWorked(db.src.insert([{x: 1}]));

            assert.commandWorked(db.adminCommand({renameCollection: srcNs, to: dstNs}));

            st.awaitReplicationOnShards();

            forEachNodeOnAllShards((node) => {
                assert.eq(
                    null,
                    getShardCatalogCollMetadata(node, srcNs),
                    `${node.host}: unexpected shard catalog entry for source namespace`,
                );
                assert.eq(
                    null,
                    getShardCatalogCollMetadata(node, dstNs),
                    `${node.host}: unexpected shard catalog entry for final namespace`,
                );
                assertInMemoryMetadataNotSharded(node, srcNs);
                assertInMemoryMetadataNotSharded(node, dstNs);
            });
        });

        it("unsharded collection renamed to unsharded collection, replacing it", function () {
            const db = setupDb("rename_unsharded_replace");
            const srcNs = `${db.getName()}.src`;
            const dstNs = `${db.getName()}.dst`;

            assert.commandWorked(db.src.insert([{x: 1}]));
            assert.commandWorked(db.dst.insert([{y: 2}]));

            assert.commandWorked(db.adminCommand({renameCollection: srcNs, to: dstNs, dropTarget: true}));

            st.awaitReplicationOnShards();

            forEachNodeOnAllShards((node) => {
                assert.eq(
                    null,
                    getShardCatalogCollMetadata(node, srcNs),
                    `${node.host}: unexpected shard catalog entry for source namespace`,
                );
                assert.eq(
                    null,
                    getShardCatalogCollMetadata(node, dstNs),
                    `${node.host}: unexpected shard catalog entry for final namespace`,
                );
                assertInMemoryMetadataNotSharded(node, srcNs);
                assertInMemoryMetadataNotSharded(node, dstNs);
            });
        });

        it("sharded collection renamed to unsharded collection namespace, replacing it", function () {
            const db = setupDb("rename_sharded_to_unsharded");
            const srcNs = `${db.getName()}.src`;
            const dstNs = `${db.getName()}.dst`;

            assert.commandWorked(db.adminCommand({shardCollection: srcNs, key: {x: 1}}));
            assert.commandWorked(db.adminCommand({split: srcNs, middle: {x: 0}}));
            assert.commandWorked(
                db.adminCommand({moveChunk: srcNs, find: {x: 0}, to: st.shard1.shardName, _waitForDelete: true}),
            );
            assert.commandWorked(db.src.insert([{x: -1}, {x: 1}]));
            assert.commandWorked(db.dst.insert([{y: 1}]));

            const srcMeta = getGlobalCatalogCollMetadata(srcNs);
            const srcUuid = srcMeta.uuid;
            const srcKey = srcMeta.key;

            assert.commandWorked(db.adminCommand({renameCollection: srcNs, to: dstNs, dropTarget: true}));

            st.awaitReplicationOnShards();

            [
                {rs: st.rs0, shardName: st.shard0.shardName},
                {rs: st.rs1, shardName: st.shard1.shardName},
            ].forEach(({rs, shardName}) => {
                const shardGlobalChunks = getGlobalCatalogChunks(srcUuid, shardName);
                assert.gt(shardGlobalChunks.length, 0, `Expected at least one chunk on ${shardName}`);

                rs.nodes.forEach((node) => {
                    // Shard catalog has entries for the final namespace.
                    assertShardCatalogOnNode(node, dstNs, {
                        expectedUuid: srcUuid,
                        expectedKey: srcKey,
                        expectedChunks: shardGlobalChunks,
                    });
                    // No entries for the old source namespace.
                    assert.eq(
                        null,
                        getShardCatalogCollMetadata(node, srcNs),
                        `${node.host}: unexpected shard catalog entry for old source namespace`,
                    );
                });

                assertInMemoryMetadataSharded(rs.getPrimary(), dstNs, srcKey);
                assertInMemoryMetadataNotSharded(rs.getPrimary(), srcNs);
            });
        });

        it("sharded collection renamed to new namespace without replacing", function () {
            const db = setupDb("rename_sharded_no_replace");
            const srcNs = `${db.getName()}.src`;
            const dstNs = `${db.getName()}.dst`;

            assert.commandWorked(db.adminCommand({shardCollection: srcNs, key: {x: 1}}));
            assert.commandWorked(db.adminCommand({split: srcNs, middle: {x: 0}}));
            assert.commandWorked(
                db.adminCommand({moveChunk: srcNs, find: {x: 0}, to: st.shard1.shardName, _waitForDelete: true}),
            );
            assert.commandWorked(db.src.insert([{x: -1}, {x: 1}]));

            const srcMeta = getGlobalCatalogCollMetadata(srcNs);
            const srcUuid = srcMeta.uuid;
            const srcKey = srcMeta.key;

            assert.commandWorked(db.adminCommand({renameCollection: srcNs, to: dstNs}));

            st.awaitReplicationOnShards();

            [
                {rs: st.rs0, shardName: st.shard0.shardName},
                {rs: st.rs1, shardName: st.shard1.shardName},
            ].forEach(({rs, shardName}) => {
                const shardGlobalChunks = getGlobalCatalogChunks(srcUuid, shardName);
                assert.gt(shardGlobalChunks.length, 0, `Expected at least one chunk on ${shardName}`);

                rs.nodes.forEach((node) => {
                    // Shard catalog has entries for the final namespace.
                    assertShardCatalogOnNode(node, dstNs, {
                        expectedUuid: srcUuid,
                        expectedKey: srcKey,
                        expectedChunks: shardGlobalChunks,
                    });
                    // No entries for the old source namespace.
                    assert.eq(
                        null,
                        getShardCatalogCollMetadata(node, srcNs),
                        `${node.host}: unexpected shard catalog entry for old source namespace`,
                    );
                });

                assertInMemoryMetadataSharded(rs.getPrimary(), dstNs, srcKey);
                assertInMemoryMetadataNotSharded(rs.getPrimary(), srcNs);
            });
        });

        it("tracked unsplittable collection renamed across databases", function () {
            const srcDb = setupDb("rename_unsplittable_cross_src");
            const srcNs = `${srcDb.getName()}.coll`;
            const dstDbName = uniqueDbName("rename_unsplittable_cross_dst");
            const dstDb = st.s.getDB(dstDbName);
            const dstNs = `${dstDbName}.coll`;
            assert.commandWorked(dstDb.adminCommand({enableSharding: dstDbName, primaryShard: st.shard0.shardName}));
            assert.commandWorked(
                dstDb.adminCommand({enableSharding: srcDb.getName(), primaryShard: st.shard0.shardName}),
            );

            // Make the collection tracked-but-unsplittable
            assert.commandWorked(srcDb.coll.insert([{x: 1}, {x: 2}]));
            assert.commandWorked(srcDb.adminCommand({moveCollection: srcNs, toShard: st.shard0.shardName}));

            const originalUuid = getGlobalCatalogCollMetadata(srcNs).uuid;

            assert.commandWorked(srcDb.adminCommand({renameCollection: srcNs, to: dstNs}));

            // Cross-DB rename must reissue the collection UUID.
            const newMeta = getGlobalCatalogCollMetadata(dstNs);
            assert.neq(null, newMeta, `${dstNs}: missing in global catalog after rename`);
            assert.neq(
                originalUuid.toString(),
                newMeta.uuid.toString(),
                "cross-DB rename must reissue the collection UUID",
            );
            assert.eq(null, getGlobalCatalogCollMetadata(srcNs), `${srcNs}: still in global catalog after rename`);

            st.awaitReplicationOnShards();

            // Old UUID has no chunks anywhere and no node retains an entry for the source namespace.
            forEachNodeOnAllShards((node) => {
                assert.eq(
                    0,
                    getShardCatalogChunks(node, originalUuid).length,
                    `${node.host}: stale chunks for original uuid still present`,
                );
                assert.eq(
                    null,
                    getShardCatalogCollMetadata(node, srcNs),
                    `${node.host}: unexpected shard catalog entry for old source namespace`,
                );
                assertInMemoryMetadataNotSharded(node, srcNs);
            });

            // Locate the data shard from the new chunk's placement.
            const newGlobalChunks = getAllGlobalCatalogChunks(newMeta.uuid);
            assert.eq(1, newGlobalChunks.length, `Expected 1 chunk for ${dstNs} after rename`);

            // Verify the shard now carries the new UUID with the authoritative chunks under the new namespace.
            st.rs0.nodes.forEach((node) => {
                assertShardCatalogOnNode(node, dstNs, {
                    expectedUuid: newMeta.uuid,
                    expectedKey: {_id: 1},
                    expectedChunks: newGlobalChunks,
                });
            });
        });

        it("sharded collection renamed to another sharded collection namespace, replacing it", function () {
            const db = setupDb("rename_sharded_to_sharded");
            const srcNs = `${db.getName()}.src`;
            const dstNs = `${db.getName()}.dst`;

            assert.commandWorked(db.adminCommand({shardCollection: srcNs, key: {x: 1}}));
            assert.commandWorked(db.adminCommand({split: srcNs, middle: {x: 0}}));
            assert.commandWorked(
                db.adminCommand({moveChunk: srcNs, find: {x: 0}, to: st.shard1.shardName, _waitForDelete: true}),
            );
            assert.commandWorked(db.src.insert([{x: -1}, {x: 1}]));

            assert.commandWorked(db.adminCommand({shardCollection: dstNs, key: {y: 1}}));
            assert.commandWorked(db.dst.insert([{y: 1}]));

            const srcMeta = getGlobalCatalogCollMetadata(srcNs);
            const srcUuid = srcMeta.uuid;
            const srcKey = srcMeta.key;
            const originalDstUuid = getGlobalCatalogCollMetadata(dstNs).uuid;

            assert.commandWorked(db.adminCommand({renameCollection: srcNs, to: dstNs, dropTarget: true}));

            st.awaitReplicationOnShards();

            [
                {rs: st.rs0, shardName: st.shard0.shardName},
                {rs: st.rs1, shardName: st.shard1.shardName},
            ].forEach(({rs, shardName}) => {
                const shardGlobalChunks = getGlobalCatalogChunks(srcUuid, shardName);
                assert.gt(shardGlobalChunks.length, 0, `Expected at least one chunk on ${shardName}`);

                rs.nodes.forEach((node) => {
                    // Shard catalog has entries for the final namespace with the source's metadata.
                    assertShardCatalogOnNode(node, dstNs, {
                        expectedUuid: srcUuid,
                        expectedKey: srcKey,
                        expectedChunks: shardGlobalChunks,
                    });
                    // No entries for the old source namespace.
                    assert.eq(
                        null,
                        getShardCatalogCollMetadata(node, srcNs),
                        `${node.host}: unexpected shard catalog entry for old source namespace`,
                    );
                    // No chunks for the replaced collection's UUID.
                    assert.eq(
                        0,
                        getShardCatalogChunks(node, originalDstUuid).length,
                        `${node.host}: unexpected chunks for replaced collection UUID`,
                    );
                });

                assertInMemoryMetadataSharded(rs.getPrimary(), dstNs, srcKey);
                assertInMemoryMetadataNotSharded(rs.getPrimary(), srcNs);
            });
        });
    });
});
