/*
 * Tests the playbook for restoring consistent collection and chunk metadata.
 *
 * Each scenario corrupts one layer, confirms checkMetadataConsistency flags it, then applies the
 * playbook: edit the affected catalog(s) with majority write concern, restart affected shards to
 * clear their CSS, and flushRouterConfig everywhere.
 *
 * Marked 'requires_persistence' (off the 'inMemory' variant): the restart step relies on durable
 * metadata surviving on disk.
 * @tags: [
 *   requires_fcv_90,
 *   requires_persistence,
 * ]
 */

import {after, afterEach, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("Authoritative collection and chunk metadata restore playbook", function () {
    const kShardCatalogCollNs = "shard.catalog.collections";
    const kShardCatalogChunksNs = "shard.catalog.chunks";
    const testDocs = [{x: -1}, {x: 1}];

    let st;
    let configDB;
    let testCounter = 0;

    function uniqueDbName(suffix) {
        // Keep the database name short: jsTestName() is the (long) file name and would exceed the
        // maximum database name length once combined with the per-scenario suffix.
        return `coll_meta_${suffix}_${testCounter++}`;
    }

    function getCollFromGlobalCatalog(ns) {
        return st.s.getDB("config").collections.findOne({_id: ns});
    }

    function getCollFromShardCatalog(node, ns) {
        return node.getDB("config").getCollection(kShardCatalogCollNs).findOne({_id: ns});
    }

    function getChunksFromShardCatalog(node, uuid) {
        return node
            .getDB("config")
            .getCollection(kShardCatalogChunksNs)
            .find({uuid: uuid})
            .sort({min: 1})
            .toArray();
    }

    function getChunksFromGlobalCatalog(uuid) {
        return st.s.getDB("config").chunks.find({uuid: uuid}).sort({min: 1}).toArray();
    }

    // Edit the global catalog (config.collections) with majority write concern. This is the CSRS-side
    // write of the playbook ("bring the CSRS to the correct state").
    function updateGlobalCollectionCatalog(ns, update) {
        assert.commandWorked(
            configDB.collections.update({_id: ns}, update, {writeConcern: {w: "majority"}}),
        );
    }

    // Edit the global catalog (config.chunks) directly on the config server replica set with majority
    // write concern. config.chunks is owned by the CSRS, so the patch is applied there (the same
    // CSRS-side write as the playbook's "bring the CSRS to the correct state" step).
    function updateGlobalChunkCatalog(query, update) {
        assert.commandWorked(
            st.configRS
                .getPrimary()
                .getDB("config")
                .chunks.update(query, update, {writeConcern: {w: "majority"}}),
        );
        st.configRS.awaitReplication();
    }

    // Apply a durable change to a single shard's per-shard catalog with majority write concern and
    // wait for it to replicate to every node ("bring the shards to the correct state" / note the
    // optime and make sure the secondaries have it). No restart here: this only edits the durable
    // layer. The in-memory CSS is cleared separately, by restarting the shard.
    function updateShardCatalog(shardRs, collectionName, query, update) {
        assert.commandWorked(
            shardRs
                .getPrimary()
                .getDB("config")
                .getCollection(collectionName)
                .update(query, update, {writeConcern: {w: "majority"}}),
        );
        shardRs.awaitReplication();
    }

    // Clear the in-memory CSS of an affected shard by restarting all of its nodes, then drive a
    // versioned read so onShardVersionMismatch rebuilds the CSS. This is the playbook's shard-side
    // refresh; it deliberately avoids the deprecated _flushRoutingTableCacheUpdates command.
    function restartShardToClearInMemoryCache(shardNum, dbName, collName) {
        st.restartShardRS(shardNum, true /* waitForPrimary */);
        assert.commandWorked(st.s.getDB(dbName).runCommand({find: collName, filter: {}}));
    }

    // Bring routing information to the correct state: run flushRouterConfig on every cluster node
    // that can cache routing info -- every mongos, the config server replica set (router role) and
    // every shard replica set (router role).
    function flushRoutingCacheEverywhere(ns) {
        const cmd = {flushRouterConfig: ns};
        st.configRS.nodes.forEach((node) => assert.commandWorked(node.adminCommand(cmd)));
        st.rs0.nodes.forEach((node) => assert.commandWorked(node.adminCommand(cmd)));
        st.rs1.nodes.forEach((node) => assert.commandWorked(node.adminCommand(cmd)));
        st.forEachMongos((node) => assert.commandWorked(node.adminCommand(cmd)));
    }

    function getInconsistencies(db) {
        // Since this test is introducing inconsistencies deliberately, secondaries must be checked at
        // the primary's timestamp, otherwise they could be checked at a timestamp where the database
        // was still inconsistent.
        const inconsistencies = db
            .checkMetadataConsistency({_checkSecondariesMode: "checkAtPrimaryTimestamp"})
            .toArray();
        jsTest.log.info("checkMetadataConsistency result", {inconsistencies});
        return inconsistencies;
    }

    function assertConsistent(db) {
        assert.eq(0, getInconsistencies(db).length);
    }

    function assertHasCollectionMetadataInconsistency(db) {
        const inconsistencies = getInconsistencies(db);
        assert(
            inconsistencies.some((i) => i.type === "InconsistentShardCatalogCollectionMetadata"),
            `expected an InconsistentShardCatalogCollectionMetadata inconsistency, found ${tojson(
                inconsistencies,
            )}`,
        );
    }

    // Create a tracked, sharded collection split into two chunks that both live on the DB primary
    // shard (shard0). shardCollection/split commit the metadata authoritatively, so shard0 is
    // authoritative for the collection and owns every chunk.
    function setupShardedCollectionOnPrimary(db, ns) {
        assert.commandWorked(
            db.adminCommand({enableSharding: db.getName(), primaryShard: st.shard0.shardName}),
        );
        assert.commandWorked(db.adminCommand({shardCollection: ns, key: {x: 1}}));
        assert.commandWorked(db.adminCommand({split: ns, middle: {x: 0}}));
        assert.commandWorked(db[ns.split(".")[1]].insert(testDocs));
        st.awaitReplicationOnShards();
    }

    function assertDataStillServed(db, collName) {
        assert.sameMembers(db[collName].find({}, {_id: 0, x: 1}).toArray(), testDocs);
        assert.commandWorked(db[collName].insert({x: 2}));
    }

    before(function () {
        st = new ShardingTest({shards: 2, rs: {nodes: 1}, config: 1, mongos: 1});
        configDB = st.s.getDB("config");
    });

    after(function () {
        st.stop();
    });

    // After every scenario the whole cluster must be back to a consistent state across all three
    // metadata layers.
    afterEach(function () {
        const inconsistencies = getInconsistencies(st.s.getDB("admin"));
        assert.eq(
            0,
            inconsistencies.length,
            `cluster metadata inconsistencies found: ${tojson(inconsistencies)}`,
        );
    });

    describe("global catalog (CSRS) holds the wrong collection metadata", function () {
        it("is fixed by editing config.collections and flushing the routers", function () {
            const dbName = uniqueDbName("global_coll");
            const collName = "coll";
            const ns = `${dbName}.${collName}`;
            const db = st.s.getDB(dbName);
            setupShardedCollectionOnPrimary(db, ns);
            assertConsistent(db);

            // Diverge the global catalog from the (correct) per-shard catalog by disallowing
            // migrations. 'allowMigrations' is compared by checkMetadataConsistency, so the
            // authoritative shard reports the divergence (both its in-memory CSS and its durable
            // catalog still hold the correct value).
            updateGlobalCollectionCatalog(ns, {$set: {allowMigrations: false}});
            assertHasCollectionMetadataInconsistency(db);

            // Playbook: bring the CSRS to the correct state, then flush the routers. The shards were
            // always correct, so no shard needs a durable edit or a restart.
            updateGlobalCollectionCatalog(ns, {$unset: {allowMigrations: ""}});
            flushRoutingCacheEverywhere(ns);

            assertConsistent(db);
            assertDataStillServed(db, collName);
        });
    });

    describe("global catalog (CSRS) holds the wrong chunk metadata", function () {
        it("is fixed by editing config.chunks and flushing the routers", function () {
            const dbName = uniqueDbName("global_chunk");
            const collName = "coll";
            const ns = `${dbName}.${collName}`;
            const db = st.s.getDB(dbName);
            setupShardedCollectionOnPrimary(db, ns);
            assertConsistent(db);

            const uuid = getCollFromGlobalCatalog(ns).uuid;
            const globalChunks = getChunksFromGlobalCatalog(uuid);
            assert.gt(globalChunks.length, 1, "expected more than one chunk in the global catalog");
            const targetChunk = globalChunks[0];
            const originalMax = targetChunk.max;

            // Shrink the upper boundary of the lowest chunk in the global catalog so its owned-chunk
            // domain no longer matches the (correct) per-shard catalog. The chunk version is left
            // untouched, so the only divergence is the chunk-domain coverage gap, which the
            // authoritative shard reports against config.chunks.
            updateGlobalChunkCatalog({_id: targetChunk._id}, {$set: {max: {x: -5}}});
            assertHasCollectionMetadataInconsistency(db);

            // Playbook: bring the CSRS to the correct state, then flush the routers. The shards were
            // always correct, so no shard needs a durable edit or a restart.
            updateGlobalChunkCatalog({_id: targetChunk._id}, {$set: {max: originalMax}});
            flushRoutingCacheEverywhere(ns);

            assertConsistent(db);
            assertDataStillServed(db, collName);
        });
    });

    describe("a shard's durable per-shard catalog holds the wrong metadata", function () {
        it("config.shard.catalog.collections on the owning DB primary shard is fixed", function () {
            const dbName = uniqueDbName("durable_coll_primary");
            const collName = "coll";
            const ns = `${dbName}.${collName}`;
            const db = st.s.getDB(dbName);
            setupShardedCollectionOnPrimary(db, ns);
            assertConsistent(db);

            // Diverge only the durable per-shard catalog on the owning shard. Detection happens while
            // shard0 is still authoritative (from the setup DDL), so the durable shard-catalog check
            // runs and flags the divergence from the (correct) global catalog.
            updateShardCatalog(
                st.rs0,
                kShardCatalogCollNs,
                {_id: ns},
                {$set: {allowMigrations: false}},
            );
            assertHasCollectionMetadataInconsistency(db);

            // Playbook: bring the affected shard's durable catalog to the correct state with majority
            // write concern. Verify the durable fix while the shard is still authoritative.
            updateShardCatalog(
                st.rs0,
                kShardCatalogCollNs,
                {_id: ns},
                {$unset: {allowMigrations: ""}},
            );
            assertConsistent(db);

            // Playbook: clear the affected shard's in-memory cache by restarting it, then flush the
            // routers everywhere.
            restartShardToClearInMemoryCache(0, dbName, collName);
            flushRoutingCacheEverywhere(ns);

            assertConsistent(db);
            assertDataStillServed(db, collName);
        });

        it("config.shard.catalog.chunks on the owning DB primary shard is fixed", function () {
            const dbName = uniqueDbName("durable_chunk_primary");
            const collName = "coll";
            const ns = `${dbName}.${collName}`;
            const db = st.s.getDB(dbName);
            setupShardedCollectionOnPrimary(db, ns);
            assertConsistent(db);

            const uuid = getCollFromGlobalCatalog(ns).uuid;
            const shardChunks = getChunksFromShardCatalog(st.rs0.getPrimary(), uuid);
            assert.gt(shardChunks.length, 1, "expected shard0 to own multiple chunks");
            const targetChunk = shardChunks[0];
            const originalMax = targetChunk.max;

            // Shrink the upper boundary of the lowest chunk so the durable chunk domain no longer
            // covers the same key space as the global catalog. The chunk version is untouched, so the
            // only divergence is the chunk-domain coverage (a gap), reported by checkMetadataConsistency.
            // The in-memory CSS is intentionally left untouched (a durable edit does not propagate to
            // it); the durable chunk check reads config.shard.catalog.chunks directly.
            updateShardCatalog(
                st.rs0,
                kShardCatalogChunksNs,
                {_id: targetChunk._id},
                {$set: {max: {x: -5}}},
            );
            assertHasCollectionMetadataInconsistency(db);

            // Playbook: restore the durable chunk boundary, verify the durable fix, then clear the
            // in-memory cache by restarting and flush the routers.
            updateShardCatalog(
                st.rs0,
                kShardCatalogChunksNs,
                {_id: targetChunk._id},
                {$set: {max: originalMax}},
            );
            assertConsistent(db);

            restartShardToClearInMemoryCache(0, dbName, collName);
            flushRoutingCacheEverywhere(ns);

            assertConsistent(db);
            assertDataStillServed(db, collName);
        });

        it("config.shard.catalog.collections on a non-primary shard that owns chunks is fixed", function () {
            const dbName = uniqueDbName("durable_coll_recipient");
            const collName = "coll";
            const ns = `${dbName}.${collName}`;
            const db = st.s.getDB(dbName);
            setupShardedCollectionOnPrimary(db, ns);

            // Move the upper chunk to the non-primary shard. moveRange is authoritative, so the
            // migration commits the collection (and the moved chunk) into the recipient's per-shard
            // catalog and marks its CSS authoritative -- exactly the state the playbook reasons about
            // for a non-primary shard that owns chunks. No extra chunk op is needed to make the
            // recipient authoritative.
            assert.commandWorked(
                db.adminCommand({moveRange: ns, min: {x: 0}, toShard: st.shard1.shardName}),
            );
            st.awaitReplicationOnShards();
            assertConsistent(db);
            assert.neq(
                null,
                getCollFromShardCatalog(st.rs1.getPrimary(), ns),
                "expected recipient shard1 to own an authoritative per-shard catalog entry",
            );

            // Diverge the durable per-shard catalog on the recipient (non-primary, currently owns
            // chunks). shard1 is authoritative for the collection, so the durable check flags it.
            updateShardCatalog(
                st.rs1,
                kShardCatalogCollNs,
                {_id: ns},
                {$set: {allowMigrations: false}},
            );
            assertHasCollectionMetadataInconsistency(db);

            // Playbook: fix the affected (recipient) shard's durable catalog, verify, restart it to
            // clear its in-memory cache, and flush the routers.
            updateShardCatalog(
                st.rs1,
                kShardCatalogCollNs,
                {_id: ns},
                {$unset: {allowMigrations: ""}},
            );
            assertConsistent(db);

            restartShardToClearInMemoryCache(1, dbName, collName);
            flushRoutingCacheEverywhere(ns);

            assertConsistent(db);
            assertDataStillServed(db, collName);
        });

        it("config.shard.catalog.chunks on a shard that historically owned a chunk is fixed", function () {
            const dbName = uniqueDbName("durable_chunk_history");
            const collName = "coll";
            const ns = `${dbName}.${collName}`;
            const db = st.s.getDB(dbName);
            setupShardedCollectionOnPrimary(db, ns);

            // Move the upper chunk to shard1 while shard0 keeps owning the lower chunk. Within the
            // snapshot history window, shard0 retains the moved-away chunk in its durable per-shard
            // catalog -- now owned by shard1 but with shard0 still recorded in the chunk's 'history'
            // so snapshot reads stay serviceable. shard0 is thus an "affected" shard for that chunk
            // even though it no longer currently owns it.
            assert.commandWorked(
                db.adminCommand({moveRange: ns, min: {x: 0}, toShard: st.shard1.shardName}),
            );
            st.awaitReplicationOnShards();
            assertConsistent(db);

            const uuid = getCollFromGlobalCatalog(ns).uuid;
            const movedChunk = getChunksFromShardCatalog(st.rs0.getPrimary(), uuid).find(
                (c) => c.shard === st.shard1.shardName,
            );
            assert.neq(
                undefined,
                movedChunk,
                "expected shard0 to retain the moved-away chunk in its durable per-shard catalog",
            );
            assert(
                movedChunk.history.some((h) => h.shard === st.shard0.shardName),
                `expected shard0 to remain in the moved chunk's history, found ${tojson(movedChunk)}`,
            );

            // Corrupt the retained chunk's history on shard0 so it no longer records shard0 as a past
            // owner. checkMetadataConsistency then flags a chunk the shard catalog stores but that the
            // shard never owned (the historically-owned chunk check on the affected shard).
            const brokenHistory = movedChunk.history.filter((h) => h.shard !== st.shard0.shardName);
            updateShardCatalog(
                st.rs0,
                kShardCatalogChunksNs,
                {_id: movedChunk._id},
                {$set: {history: brokenHistory}},
            );
            assertHasCollectionMetadataInconsistency(db);

            // Playbook: restore the affected shard's durable chunk history, verify, restart it to
            // clear its in-memory cache, and flush the routers.
            updateShardCatalog(
                st.rs0,
                kShardCatalogChunksNs,
                {_id: movedChunk._id},
                {$set: {history: movedChunk.history}},
            );
            assertConsistent(db);

            restartShardToClearInMemoryCache(0, dbName, collName);
            flushRoutingCacheEverywhere(ns);

            assertConsistent(db);
            assertDataStillServed(db, collName);
        });
    });
});
