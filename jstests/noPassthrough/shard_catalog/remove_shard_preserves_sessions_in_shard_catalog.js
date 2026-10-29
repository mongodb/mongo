/**
 * Verifies that when a shard is removed, dropShardCatalogMetadata preserves the
 * config.system.sessions entry in config.shard.catalog.collections while removing
 * all other collection entries. Dropping the sessions entry would leave the config
 * server in an invalid kUnknown state after transitionToDedicated followed by
 * transitionFromDedicated, while it still being the DB primary shard.
 *
 * @tags: [
 *   requires_fcv_90,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {removeShard} from "jstests/sharding/libs/remove_shard_util.js";

describe("removeShard preserves config.system.sessions in the shard catalog", function () {
    const kSessionsNs = "config.system.sessions";
    const kShardCatalogColls = "shard.catalog.collections";
    const dbName = "testDB";
    const collName = "testColl";
    const userNs = `${dbName}.${collName}`;

    before(() => {
        this.st = new ShardingTest({
            name: jsTestName(),
            shards: 2,
            other: {configShard: true, enableBalancer: true},
        });

        // Initializing system.sessions and custom testDB.testColl collections
        assert.commandWorked(this.st.s.adminCommand({refreshLogicalSessionCacheNow: 1}));
        assert.commandWorked(
            this.st.s.adminCommand({enableSharding: dbName, primaryShard: "config"}),
        );
        assert.commandWorked(this.st.s.adminCommand({shardCollection: userNs, key: {_id: 1}}));
        assert.commandWorked(this.st.s.getDB(dbName)[collName].insert({_id: 1}));
    });

    after(() => {
        this.st.stop();
    });

    it("keeps the sessions entry and drops the user collection entry", function () {
        const shardCatalogColls = this.st.configRS
            .getPrimary()
            .getDB("config")
            .getCollection(kShardCatalogColls);

        // Both collection entries should exist before the shard removal
        assert.neq(
            null,
            shardCatalogColls.findOne({_id: kSessionsNs}),
            "sessions entry missing from shard catalog before removal",
        );
        assert.neq(
            null,
            shardCatalogColls.findOne({_id: userNs}),
            "user collection entry missing from shard catalog before removal",
        );

        // Move data off the config shard so draining can complete
        assert.commandWorked(
            this.st.s.adminCommand({
                moveChunk: userNs,
                find: {_id: 1},
                to: this.st.shard1.shardName,
            }),
        );
        assert.commandWorked(
            this.st.s.adminCommand({movePrimary: dbName, to: this.st.shard1.shardName}),
        );

        removeShard(this.st, "config");

        // After removal system.sessions collection entry should be preserved,
        // while all other collection entries, including the user collection
        // should be dropped.
        assert.neq(
            null,
            shardCatalogColls.findOne({_id: kSessionsNs}),
            "sessions entry should be preserved in the shard catalog after removal",
        );
        assert.eq(
            null,
            shardCatalogColls.findOne({_id: userNs}),
            "user collection entry should be dropped from the shard catalog after removal",
        );
        assert.eq(
            1,
            shardCatalogColls.count(),
            "only sessions entry should be preserved in shard catalog after removal",
        );
    });
});
