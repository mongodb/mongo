/**
 * Tests that if during movePrimary the destination shard starts draining in a separate session,
 * the movePrimary aborts with ShardNotFound.
 *
 * The test pauses movePrimary just before it commits the database-primary metadata to the config
 * server, then marks the destination shard as draining via startShardDraining. When movePrimary
 * resumes, ConfigsvrCommitMovePrimary validates that the destination shard is in draining mode and
 * throws ShardNotFound.
 *
 * @tags: [
 *   does_not_support_stepdowns,
 *   featureFlagAuthoritativeShardsDDL,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("movePrimary with destination shard remove", function () {
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

        // When movePrimary resumes it should detect that destination shard is draining and abort
        awaitMovePrimary();

        // Primary didn't change
        const dbEntry = st.s.getDB("config").databases.findOne({_id: dbName});
        assert.eq(dbEntry.primary, st.shard0.shardName);

        assert.eq(drainingShard.getCollection(untrackedCollNs).find().itcount(), 0);
        assert.eq(
            primaryShard.getCollection(untrackedCollNs).find().itcount(),
            expectedDocs.length,
        );

        // Verify that both collections return the expected docs
        assert.sameMembers(
            st.s.getCollection(untrackedCollNs).find({}, {_id: 0}).toArray(),
            expectedDocs,
        );
        assert.sameMembers(
            st.s.getCollection(shardedCollNs).find({}, {_id: 0}).toArray(),
            expectedDocs,
        );

        assert.commandWorked(st.s.adminCommand({stopShardDraining: drainingShard.shardName}));
    });
});
