/**
 * Verifies that listCollections reports info.configDebugDump == true for the authoritative
 * shard-local catalog collections (config.shard.catalog.databases, config.shard.catalog.collections
 * and config.shard.catalog.chunks) on a shard, so that these collections are captured in a
 * full-cluster debug dump. The non-authoritative shard caches (config.cache.*) must instead be
 * reported as configDebugDump == false, as they are rebuilt and must never be dumped.
 *
 * @tags: [
 *   featureFlagAuthoritativeShardsDDL,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("configDebugDump for the authoritative shard catalog", function () {
    const kShardCatalogCollections = ["shard.catalog.databases", "shard.catalog.collections", "shard.catalog.chunks"];
    const kCacheCollections = ["cache.collections", "cache.databases"];

    // Returns a map from each config-db collection name to its info.configDebugDump value, as
    // reported by listCollections run directly against the given node.
    function getConfigDebugDumpByCollection(node) {
        const configDB = node.getDB("config");
        const res = assert.commandWorked(configDB.runCommand({listCollections: 1}));
        const entries = new DBCommandCursor(configDB, res).toArray();
        const byName = {};
        for (const entry of entries) {
            byName[entry.name] = entry.info ? entry.info.configDebugDump : undefined;
        }
        return byName;
    }

    before(() => {
        this.st = new ShardingTest({mongos: 1, shards: 2, rs: {nodes: 1}});

        this.dbName = jsTestName();
        this.collName = "coll";
        this.ns = `${this.dbName}.${this.collName}`;
        this.primaryShardName = this.st.shard0.shardName;
        this.shardPrimary = this.st.rs0.getPrimary();

        // Creating the database and sharding a collection commits the authoritative shard catalog
        // (config.shard.catalog.{databases,collections,chunks}) on the owning shard.
        assert.commandWorked(
            this.st.s.adminCommand({enableSharding: this.dbName, primaryShard: this.primaryShardName}),
        );
        assert.commandWorked(this.st.s.adminCommand({shardCollection: this.ns, key: {x: 1}}));
    });

    after(() => {
        this.st.stop();
    });

    it("reports configDebugDump == true for the authoritative shard catalog collections", () => {
        const byName = getConfigDebugDumpByCollection(this.shardPrimary);

        for (const coll of kShardCatalogCollections) {
            assert(coll in byName, `Expected listCollections on the shard's config db to report ${coll}`, {byName});
            assert.eq(
                "boolean",
                typeof byName[coll],
                `Expected info.configDebugDump to be a boolean for config.${coll}`,
                {byName},
            );
            assert.eq(true, byName[coll], `Expected config.${coll} to be flagged for the debug dump`, {byName});
        }
    });

    it("reports configDebugDump == false for the non-authoritative shard caches", () => {
        const byName = getConfigDebugDumpByCollection(this.shardPrimary);

        for (const coll of kCacheCollections) {
            // The caches are lazily created, so only assert on them when present.
            if (coll in byName) {
                assert.eq(
                    false,
                    byName[coll],
                    `Expected the non-authoritative cache config.${coll} to be excluded from the debug dump`,
                    {byName},
                );
            }
        }
    });
});
