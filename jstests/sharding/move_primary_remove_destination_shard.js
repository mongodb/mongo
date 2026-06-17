/**
 * Tests that if during movePrimary the destination shard starts draining in a separate session,
 * the movePrimary aborts with ShardNotFound.
 *
 * The test pauses movePrimary just before it commits the database-primary metadata to the config
 * server, then marks the destination shard as draining via startShardDraining. When movePrimary
 * resumes, ConfigsvrCommitMovePrimary validates that the destination shard is not draining and
 * throws ShardNotFound.
 *
 * @tags: [
 *   does_not_support_stepdowns,
 *   featureFlagAuthoritativeShardsDDL,
 *   featureFlagAuthoritativeShardsCRUD,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("movePrimary with destination shard remove", function () {
    let st;
    const dbName = "testDb";
    const shardedCollNs = `${dbName}.shardedData`;

    before(function () {
        st = new ShardingTest({
            shards: 2,
            rs: {nodes: 3},
        });

        assert.commandWorked(
            st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
        );

        // Tracked collection on primary shard
        assert.commandWorked(st.s.adminCommand({shardCollection: shardedCollNs, key: {x: 1}}));
        assert.commandWorked(
            st.s.getDB(dbName).shardedData.insert([{x: 1}, {x: 2}, {x: 3}, {x: 4}, {x: 5}]),
        );

        // Untracked collection on the primary shard
        assert.commandWorked(st.s.getDB(dbName).untrackedData.insert([{a: 1}, {a: 2}, {a: 3}]));
    });

    after(function () {
        st.stop();
    });

    it("aborts when destination shard starts draining before metadata commit", function () {
        const fp = configureFailPoint(st.rs0.getPrimary(), "hangBeforeMovePrimaryCommitDbMetadata");

        const awaitMovePrimary = startParallelShell(
            funWithArgs(
                function (dbName, toShard) {
                    assert.commandFailedWithCode(
                        db.adminCommand({movePrimary: dbName, to: toShard}),
                        ErrorCodes.ShardNotFound,
                    );
                },
                dbName,
                st.shard1.shardName,
            ),
            st.s.port,
        );

        jsTest.log("Waiting for movePrimary to hit the fail point");
        fp.wait();

        // At this point movePrimary has cloned untracked collections and committed
        // collection/chunk metadata to the recipient's local shard catalog, but has
        // not yet committed to the config server.
        assert.soon(
            () => st.rs1.getPrimary().getDB(dbName).untrackedData.find().itcount() === 3,
            "untrackedData documents should be cloned to recipient shard",
        );
        assert.soon(
            () =>
                st.rs1
                    .getPrimary()
                    .getDB("config")
                    .getCollection("shard.catalog.collections")
                    .find({_id: shardedCollNs})
                    .itcount() === 1,
            "shard.catalog.collections on recipient should have an entry for the tracked collection",
        );

        jsTest.log("Marking destination shard as draining while movePrimary is paused");
        assert.commandWorked(st.s.adminCommand({startShardDraining: st.shard1.shardName}));

        jsTest.log("Resuming movePrimary after destination shard started draining");
        fp.off();

        // When movePrimary resumes it should detect that destination shard is missing
        awaitMovePrimary();

        // Primary didn't change
        const dbEntry = st.s.getDB("config").databases.findOne({_id: dbName});
        assert.eq(dbEntry.primary, st.shard0.shardName);

        // Committed untracked collections should be reverted during abort
        assert.eq(st.rs1.getPrimary().getDB(dbName).untrackedData.find().itcount(), 0);
        assert.eq(st.rs0.getPrimary().getDB(dbName).untrackedData.find().itcount(), 3);

        // Committed config.shard.catalog.collections entries should be reverted during abort
        assert.soon(
            () =>
                st.rs1
                    .getPrimary()
                    .getDB("config")
                    .getCollection("shard.catalog.collections")
                    .find({_id: new RegExp(`^${dbName}\\.`)})
                    .itcount() === 0,
            "shard.catalog.collections entries on recipient should be cleaned up after abort",
        );

        // Committed config.shard.catalog.chunks entries should be reverted during abort
        assert.eq(
            st.rs1
                .getPrimary()
                .getDB("config")
                .getCollection("shard.catalog.chunks")
                .find()
                .itcount(),
            0,
        );

        assert.commandWorked(st.s.adminCommand({stopShardDraining: st.shard1.shardName}));
    });
});
