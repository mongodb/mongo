/**
 * Verifies that when a shard is removed as part of the transition to dedicated config
 * the shard catalog on secondaries gets cleaned up as well and doesn't expose stale
 * shard catalog metadata to customers in case of step down of the primary and step up
 * of one of the secondaries after the transition.
 *
 * @tags: [
 *   requires_fcv_90,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {moveOutSessionChunks, removeShard} from "jstests/sharding/libs/remove_shard_util.js";

describe("When there was leftover shard catalog metadata in CSR of secondary during transition to dedicated config server", function () {
    const dbName = "testDB";
    const collName = "testColl";
    const userNs = `${dbName}.${collName}`;
    const kShardCatalogColls = "shard.catalog.collections";
    const kShardCatalogDbs = "shard.catalog.databases";

    before(function () {
        this.st = new ShardingTest({
            name: jsTestName(),
            shards: 2,
            config: 3,
            other: {configShard: true},
        });

        assert.commandWorked(
            this.st.s.adminCommand({enableSharding: dbName, primaryShard: "config"}),
        );
        assert.commandWorked(this.st.s.adminCommand({shardCollection: userNs, key: {_id: 1}}));
        assert.commandWorked(this.st.s.getDB(dbName)[collName].insert({_id: 1}));
    });

    after(function () {
        this.st.stop();
    });

    it("shard catalog metadata on secondaries should get cleaned up", function () {
        // Ensuring the metadata has been populated to primary
        const originalPrimary = this.st.configRS.getPrimary();
        const svBefore = assert.commandWorked(
            originalPrimary.adminCommand({getShardVersion: userNs, fullMetadata: true}),
        );
        assert(
            svBefore.metadata && svBefore.metadata.collVersion,
            "CSR should have sharded metadata on primary",
            {svBefore},
        );
        const targetSecondary = this.st.configRS.getSecondaries()[0];
        this.st.configRS.awaitReplication();

        // Setting up some leftover shard catalog metadata on secondary CSR through primary
        // step down and secondary step up
        assert.commandWorked(originalPrimary.adminCommand({replSetStepDown: 30}));
        this.st.configRS.stepUp(targetSecondary);

        // Forcing collection metadata recovery from unknown CSR state on secondary
        this.st.s.getDB(dbName)[collName].find({_id: 1}).toArray();
        assert.soon(() => {
            const sv = targetSecondary.adminCommand({getShardVersion: userNs, fullMetadata: true});
            return sv.ok && sv.metadata && sv.metadata.collVersion;
        }, "CSR should be populated with sharded metadata on secondary");

        // Stepping down with the secondary and restoring previous primary
        assert.commandWorked(targetSecondary.adminCommand({replSetStepDown: 30}));
        this.st.configRS.stepUp(originalPrimary);
        assert.eq(
            originalPrimary.host,
            this.st.configRS.getPrimary().host,
            "Original primary should be elected as the new primary again",
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
        moveOutSessionChunks(this.st, "config", this.st.shard1.shardName);

        const oplogTsBeforeRemoval = originalPrimary
            .getDB("local")
            .oplog.rs.find()
            .sort({ts: -1})
            .limit(1)
            .next().ts;

        // Transitioning to dedicated config server
        removeShard(this.st, "config");

        // Exactly one `invalidateAllCollectionMetadata` and one `invalidateAllDatabaseMetadata`
        // oplog entry should be emitted per shard removal.
        const invalidateAllCollEntries = originalPrimary
            .getDB("local")
            .oplog.rs.find({
                ts: {$gt: oplogTsBeforeRemoval},
                op: "c",
                "o.invalidateAllCollectionMetadata": {$exists: true},
            })
            .toArray();
        assert.eq(
            1,
            invalidateAllCollEntries.length,
            "expected exactly one invalidateAllCollectionMetadata oplog entry per shard removal",
            {invalidateAllCollEntries},
        );

        const invalidateAllDbEntries = originalPrimary
            .getDB("local")
            .oplog.rs.find({
                ts: {$gt: oplogTsBeforeRemoval},
                op: "c",
                "o.invalidateAllDatabaseMetadata": {$exists: true},
            })
            .toArray();
        assert.eq(
            1,
            invalidateAllDbEntries.length,
            "expected exactly one invalidateAllDatabaseMetadata oplog entry per shard removal",
            {invalidateAllDbEntries},
        );

        // Durable shard catalog should be clean on the original primary.
        assert.eq(
            null,
            originalPrimary
                .getDB("config")
                .getCollection(kShardCatalogColls)
                .findOne({_id: userNs}),
            "user collection entry should be absent from shard catalog after transition",
        );
        assert.eq(
            0,
            originalPrimary.getDB("config").getCollection(kShardCatalogDbs).find().itcount(),
            "shard catalog databases should be empty after transition",
        );
        this.st.configRS.awaitReplication();

        // Stepping down with primary and stepping up with secondary the second time
        assert.commandWorked(originalPrimary.adminCommand({replSetStepDown: 60}));
        this.st.configRS.stepUp(targetSecondary);

        // Secondary is elected as the new primary
        const newPrimary = this.st.configRS.getPrimary();
        assert.eq(
            targetSecondary.host,
            newPrimary.host,
            "targetSecondary should be the new primary after stepdown",
        );

        // Verify that both durable state and in-memory CSR state are cleaned up on secondary
        assert.eq(
            null,
            newPrimary.getDB("config").getCollection(kShardCatalogColls).findOne({_id: userNs}),
            "There is no user collection entry on the new primary",
        );
        assert.eq(
            0,
            newPrimary.getDB("config").getCollection(kShardCatalogDbs).find().itcount(),
            "Shard catalog databases should be empty on new primary",
        );

        const svAfter = assert.commandWorked(
            newPrimary.adminCommand({getShardVersion: userNs, fullMetadata: true}),
        );
        assert.eq({}, svAfter.metadata, "CSR metadata should be cleaned up on the new primary", {
            svAfter,
        });

        assert.commandWorked(this.st.s.getDB(dbName).dropDatabase());
    });
});
