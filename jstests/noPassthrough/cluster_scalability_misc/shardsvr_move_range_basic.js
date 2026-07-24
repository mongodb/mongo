/**
 * Simple test cases around _shardsvrMoveRange command.
 *
 * @tags: [
 *   featureFlagAuthoritativeShardsDDL,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {setAllowChunkOperations} from "jstests/sharding/libs/set_allow_chunk_operations_util.js";

function shardCollection(st, dbName, collName) {
    const ns = dbName + "." + collName;
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));
    return ns;
}

function makeShardsvrMoveRangeCmd(
    st,
    ns,
    fromShard,
    toShard,
    min = {x: MinKey},
    max = {x: MaxKey},
) {
    const collection = st.s.getCollection("config.collections").findOne({_id: ns});
    return {
        _shardsvrMoveRange: ns,
        fromShard,
        toShard,
        min,
        max,
        collectionTimestamp: collection.timestamp,
        maxChunkSizeBytes: 64 * 1024 * 1024,
    };
}

function assertOwningShard(st, ns, shard, min = {x: MinKey}) {
    const collection = st.s.getCollection("config.collections").findOne({_id: ns});
    const chunk = st.s.getCollection("config.chunks").findOne({uuid: collection.uuid, min});
    assert.eq(shard, chunk.shard, "chunk is not owned by the expected shard", {
        chunk,
        shard,
    });
}

describe("_shardsvrMoveRange tests", function () {
    before(function () {
        this.st = new ShardingTest({shards: 2, rs: {nodes: 2}});
        this.dbName = jsTestName();

        this.primaryShard = this.st.shard0;
        this.recipientShard = this.st.shard1;

        assert.commandWorked(
            this.st.s.adminCommand({
                enableSharding: this.dbName,
                primaryShard: this.primaryShard.shardName,
            }),
        );
    });

    after(function () {
        this.st.stop();
    });

    it("retries after a committed global catalog update", function () {
        const ns = shardCollection(this.st, this.dbName, "global_catalog_retry");
        const moveRangeCmd = makeShardsvrMoveRangeCmd(
            this.st,
            ns,
            this.primaryShard.shardName,
            this.recipientShard.shardName,
        );
        // The global catalog transaction commits before this failpoint returns an error. The
        // coordinator must retry and rebuild the result from the committed catalog state.
        const fp = configureFailPoint(
            this.st.configRS.getPrimary(),
            "commitChunkMigrationFailAfterCommit",
        );
        try {
            assert.commandWorked(this.primaryShard.adminCommand(moveRangeCmd));
        } finally {
            fp.off();
        }

        assertOwningShard(this.st, ns, this.recipientShard.shardName);
    });

    it("is a no-op when the donor and recipient are the same shard", function () {
        const ns = shardCollection(this.st, this.dbName, "same_shard");
        const moveRangeCmd = makeShardsvrMoveRangeCmd(
            this.st,
            ns,
            this.primaryShard.shardName,
            this.primaryShard.shardName,
        );

        assert.commandWorked(this.primaryShard.adminCommand(moveRangeCmd));

        assertOwningShard(this.st, ns, this.primaryShard.shardName);
    });

    it("rejects a range whose shard key does not match the collection shard key", function () {
        const ns = shardCollection(this.st, this.dbName, "invalid_shard_key");
        const moveRangeCmd = makeShardsvrMoveRangeCmd(
            this.st,
            ns,
            this.primaryShard.shardName,
            this.recipientShard.shardName,
            {aaa: 10},
            {x: MaxKey},
        );

        assert.commandFailedWithCode(
            this.primaryShard.adminCommand(moveRangeCmd),
            ErrorCodes.StaleConfig,
        );
        assertOwningShard(this.st, ns, this.primaryShard.shardName);
    });

    it("rejects a request received by a shard that does not own the chunk", function () {
        const ns = shardCollection(this.st, this.dbName, "wrong_donor");
        assert.commandWorked(this.st.s.adminCommand({split: ns, middle: {x: 0}}));
        assert.commandWorked(
            this.st.s.adminCommand({
                moveChunk: ns,
                find: {x: -1},
                to: this.recipientShard.shardName,
            }),
        );
        const moveRangeCmd = makeShardsvrMoveRangeCmd(
            this.st,
            ns,
            this.primaryShard.shardName,
            this.recipientShard.shardName,
            {x: 0},
            {x: MaxKey},
        );

        assert.commandFailedWithCode(
            this.st.rs1.getPrimary().adminCommand(moveRangeCmd),
            ErrorCodes.StaleConfig,
        );
        assertOwningShard(this.st, ns, this.primaryShard.shardName, {x: 0});
    });

    it("rejects a request when chunk operations are disabled", function () {
        const ns = shardCollection(this.st, this.dbName, "chunk_operations_disabled");
        const moveRangeCmd = makeShardsvrMoveRangeCmd(
            this.st,
            ns,
            this.primaryShard.shardName,
            this.recipientShard.shardName,
        );

        setAllowChunkOperations(this.st, ns, false);
        try {
            assert.commandFailedWithCode(
                this.primaryShard.adminCommand(moveRangeCmd),
                ErrorCodes.ConflictingOperationInProgress,
            );
        } finally {
            setAllowChunkOperations(this.st, ns, true);
        }
        assertOwningShard(this.st, ns, this.primaryShard.shardName);
    });

    it("rejects requests for nonexistent and dropped collections", function () {
        const ns = shardCollection(this.st, this.dbName, "dropped_collection");
        const moveRangeCmd = makeShardsvrMoveRangeCmd(
            this.st,
            ns,
            this.primaryShard.shardName,
            this.recipientShard.shardName,
        );

        assert.commandWorked(this.st.s.getDB(this.dbName).runCommand({drop: "dropped_collection"}));
        assert.commandFailedWithCode(
            this.primaryShard.adminCommand(moveRangeCmd),
            ErrorCodes.StaleConfig,
        );

        assert.commandFailedWithCode(
            this.primaryShard.adminCommand({
                ...moveRangeCmd,
                _shardsvrMoveRange: this.dbName + ".does_not_exist",
            }),
            ErrorCodes.StaleConfig,
        );
    });
});
