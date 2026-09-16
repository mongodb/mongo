/**
 * Tests that movePrimary runs to completion if its destination shard starts draining once the
 * coordinator has already reached the commit phase.
 *
 * The MovePrimaryCoordinator only refuses a draining destination while the operation can still be
 * rolled back, that is, before the commit phase. The test pauses movePrimary just before it commits
 * the database-primary metadata to the config server and marks the destination shard as draining
 * via startShardDraining while it is paused. From that point on the coordinator must roll forward:
 * aborting would skip the remaining phases, leaving the donor's stale collections behind and the
 * shard catalogs disagreeing with the global one. The database primary therefore ends up on the
 * draining shard, and it is up to the drain to report it as remaining work to move.
 *
 * @tags: [
 *   does_not_support_stepdowns,
 *   assumes_balancer_off,
 *   requires_fcv_90,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("movePrimary with a destination shard that starts draining", function () {
    let st;
    let primaryShard;
    let drainingShard;
    const dbName = "testDb";
    const shardedCollNs = `${dbName}.shardedColl`;
    const untrackedCollNs = `${dbName}.untrackedColl`;
    const expectedDocs = [{x: 1}, {x: 2}, {x: 3}, {x: 4}, {x: 5}];

    before(function () {
        st = new ShardingTest({
            shards: 2,
            rs: {nodes: 3},
        });

        primaryShard = st.shard0;
        drainingShard = st.shard1;

        assert.commandWorked(
            st.s.adminCommand({enableSharding: dbName, primaryShard: primaryShard.shardName}),
        );

        // Tracked collection on primary shard
        assert.commandWorked(st.s.adminCommand({shardCollection: shardedCollNs, key: {x: 1}}));
        assert.commandWorked(st.s.getCollection(shardedCollNs).insert(expectedDocs));

        // Untracked collection on the primary shard
        assert.commandWorked(st.s.getCollection(untrackedCollNs).insert(expectedDocs));
    });

    after(function () {
        st.stop();
    });

    it("completes when destination shard starts draining before metadata commit", function () {
        const fp = configureFailPoint(st.rs0.getPrimary(), "hangBeforeMovePrimaryCommitDbMetadata");

        const awaitMovePrimary = startParallelShell(
            funWithArgs(
                function (dbName, toShard) {
                    assert.commandWorked(db.adminCommand({movePrimary: dbName, to: toShard}));
                },
                dbName,
                drainingShard.shardName,
            ),
            st.s.port,
        );

        jsTest.log("Waiting for movePrimary to hit the fail point");
        fp.wait();

        // At this point movePrimary has cloned untracked collections and committed collection/chunk
        // metadata to the recipient's local shard catalog, but has not yet committed to the config
        // server.
        assert.soon(
            () =>
                drainingShard.getCollection(untrackedCollNs).find().itcount() ===
                expectedDocs.length,
            "untrackedColl documents should be cloned to recipient shard",
        );
        assert.soon(
            () =>
                drainingShard
                    .getDB("config")
                    .getCollection("shard.catalog.collections")
                    .find({_id: shardedCollNs})
                    .itcount() === 1,
            "shard.catalog.collections on recipient should have an entry for the tracked collection",
        );

        jsTest.log("Marking destination shard as draining while movePrimary is paused");
        assert.commandWorked(st.s.adminCommand({startShardDraining: drainingShard.shardName}));

        jsTest.log("Resuming movePrimary after destination shard started draining");
        fp.off();

        // The coordinator is past the point where a draining destination can stop it, so it rolls
        // forward through the remaining phases instead of aborting.
        awaitMovePrimary();

        // The primary moved to the draining shard.
        const dbEntry = st.s.getDB("config").databases.findOne({_id: dbName});
        assert.eq(dbEntry.primary, drainingShard.shardName);

        // The clean phase dropped the donor's now-stale copy, so the recipient holds the only one.
        assert.eq(
            drainingShard.getCollection(untrackedCollNs).find().itcount(),
            expectedDocs.length,
        );
        assert.eq(primaryShard.getCollection(untrackedCollNs).find().itcount(), 0);

        // Verify that both collections return the expected docs
        assert.sameMembers(
            st.s.getCollection(untrackedCollNs).find({}, {_id: 0}).toArray(),
            expectedDocs,
        );
        assert.sameMembers(
            st.s.getCollection(shardedCollNs).find({}, {_id: 0}).toArray(),
            expectedDocs,
        );

        // Rolling forward must not leave the catalogs inconsistent.
        const inconsistencies = st.s.getDB(dbName).checkMetadataConsistency().toArray();
        assert.eq(0, inconsistencies.length, "unexpected metadata inconsistencies", {
            inconsistencies,
        });

        assert.commandWorked(st.s.adminCommand({stopShardDraining: drainingShard.shardName}));
    });
});
