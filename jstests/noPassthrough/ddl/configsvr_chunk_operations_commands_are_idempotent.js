/**
 * Verifies that _configsvr* commit commands for splitChunk and mergeChunks are idempotent even
 * with {allowChunkOperations: false}.
 *
 * @tags: [
 *   requires_fcv_90,
 * ]
 */
import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";

describe("_configsvr commit chunk operations are idempotent", function () {
    before(() => {
        this.st = new ShardingTest({shards: 1});
        this.dbName = "split_merge_allow_chunk_operations_db";
        this.config = this.st.configRS.getPrimary().getDB("config");

        assert.commandWorked(
            this.st.s.adminCommand({
                enableSharding: this.dbName,
                primaryShard: this.st.shard0.shardName,
            }),
        );

        this.setAllowChunkOperations = (ns, allow) => {
            assert.commandWorked(
                this.config.adminCommand({
                    _configsvrSetAllowChunkOperations: ns,
                    allowChunkOperations: allow,
                    writeConcern: {w: "majority"},
                }),
            );
        };

        this.countChunks = (ns) =>
            findChunksUtil.findChunksByNs(this.st.s.getDB("config"), ns).itcount();

        this.checkSplitChunk = () => {
            const command = {
                _configsvrCommitChunkSplit: this.ns,
                collEpoch: this.collEpoch,
                collTimestamp: this.collTimestamp,
                shard: this.st.shard0.shardName,
                min: {x: MinKey},
                max: {x: MaxKey},
                splitPoints: [{x: 0}],
            };

            assert.eq(1, this.countChunks(this.ns));

            this.setAllowChunkOperations(this.ns, false);

            assert.commandFailedWithCode(
                this.config.adminCommand(command),
                ErrorCodes.ConflictingOperationInProgress,
            );
            assert.eq(1, this.countChunks(this.ns), "split must not have committed");

            // After re-enabling chunk operations the same split must succeed, proving the failure
            // was attributable solely to the allowChunkOperations flag.
            this.setAllowChunkOperations(this.ns, true);
            assert.commandWorked(this.config.adminCommand(command));
            assert.eq(2, this.countChunks(this.ns));
        };

        this.checkSplitChunkIdempotency = () => {
            const command = {
                _configsvrCommitChunkSplit: this.ns,
                collEpoch: this.collEpoch,
                collTimestamp: this.collTimestamp,
                shard: this.st.shard0.shardName,
            };

            assert.eq(1, this.countChunks(this.ns));

            assert.commandWorked(
                this.config.adminCommand({
                    ...command,
                    min: {x: MinKey},
                    max: {x: MaxKey},
                    splitPoints: [{x: 0}],
                }),
            );
            assert.eq(2, this.countChunks(this.ns));

            this.setAllowChunkOperations(this.ns, false);

            // The same split operation under allowChunkOperations = false returns OK.
            assert.commandWorked(
                this.config.adminCommand({
                    ...command,
                    min: {x: MinKey},
                    max: {x: MaxKey},
                    splitPoints: [{x: 0}],
                }),
            );
            assert.eq(2, this.countChunks(this.ns));

            // A different split still fails.
            assert.commandFailedWithCode(
                this.config.adminCommand({
                    ...command,
                    min: {x: 0},
                    max: {x: MaxKey},
                    splitPoints: [{x: 10}],
                }),
                ErrorCodes.ConflictingOperationInProgress,
            );
            assert.eq(2, this.countChunks(this.ns), "split must not have committed");

            this.setAllowChunkOperations(this.ns, true);
        };

        this.checkMergeChunks = () => {
            const command = {
                _configsvrCommitChunksMerge: this.ns,
                collEpoch: this.collEpoch,
                collTimestamp: this.collTimestamp,
                collUUID: this.collUUID,
                shard: this.st.shard0.shardName,
                chunkRange: {min: {x: MinKey}, max: {x: 10}},
            };

            // Pre-split into 3 chunks: [MinKey, 0), [0, 10), [10, MaxKey).
            assert.commandWorked(this.st.s.adminCommand({split: this.ns, middle: {x: 0}}));
            assert.commandWorked(this.st.s.adminCommand({split: this.ns, middle: {x: 10}}));
            assert.eq(3, this.countChunks(this.ns));

            this.setAllowChunkOperations(this.ns, false);

            assert.commandFailedWithCode(
                this.config.adminCommand(command),
                ErrorCodes.ConflictingOperationInProgress,
            );
            assert.eq(3, this.countChunks(this.ns), "mergeChunks must not have committed");

            this.setAllowChunkOperations(this.ns, true);
            assert.commandWorked(this.config.adminCommand(command));
            assert.eq(2, this.countChunks(this.ns));
        };

        this.checkMergeChunksIdempotency = () => {
            const command = {
                _configsvrCommitChunksMerge: this.ns,
                collEpoch: this.collEpoch,
                collTimestamp: this.collTimestamp,
                collUUID: this.collUUID,
                shard: this.st.shard0.shardName,
            };

            // Pre-split into 3 chunks: [MinKey, 0), [0, 10), [10, MaxKey).
            assert.commandWorked(this.st.s.adminCommand({split: this.ns, middle: {x: 0}}));
            assert.commandWorked(this.st.s.adminCommand({split: this.ns, middle: {x: 10}}));
            assert.eq(3, this.countChunks(this.ns));

            assert.commandWorked(
                this.config.adminCommand({
                    ...command,
                    chunkRange: {min: {x: MinKey}, max: {x: 10}},
                }),
            );
            assert.eq(2, this.countChunks(this.ns));

            this.setAllowChunkOperations(this.ns, false);

            // The same merge operation under allowChunkOperations = false returns OK.
            assert.commandWorked(
                this.config.adminCommand({
                    ...command,
                    chunkRange: {min: {x: MinKey}, max: {x: 10}},
                }),
            );
            assert.eq(2, this.countChunks(this.ns));

            // A different merge still fails.
            assert.commandFailedWithCode(
                this.config.adminCommand({
                    ...command,
                    chunkRange: {min: {x: MinKey}, max: {x: MaxKey}},
                }),
                ErrorCodes.ConflictingOperationInProgress,
            );
            assert.eq(2, this.countChunks(this.ns), "mergeChunks must not have committed");

            this.setAllowChunkOperations(this.ns, true);
        };
    });

    after(() => {
        this.st.stop();
    });

    beforeEach(() => {
        // Per-test namespace so chunk state and the allowChunkOperations flag are isolated.
        this.collName = "coll_" + new ObjectId().str;
        this.ns = this.dbName + "." + this.collName;
        assert.commandWorked(this.st.s.adminCommand({shardCollection: this.ns, key: {x: 1}}));
        const collectionEntries = this.st.s
            .getDB("config")
            .collections.find({_id: this.ns})
            .toArray();
        assert.eq(collectionEntries.length, 1);
        this.collEpoch = collectionEntries[0].lastmodEpoch;
        this.collTimestamp = collectionEntries[0].timestamp;
        this.collUUID = {uuid: collectionEntries[0].uuid};
    });

    afterEach(() => {
        // Re-enable chunk operations before dropping so the collection document is in the default state
        // for any other tooling that inspects history. Then drop to release resources.
        this.setAllowChunkOperations(this.ns, true);
        assert.commandWorked(this.st.s.getDB(this.dbName).runCommand({drop: this.collName}));
    });

    it("rejects splitChunk when allowChunkOperations is false", () => {
        this.checkSplitChunk();
    });

    it("rejects mergeChunks when allowChunkOperations is false", () => {
        this.checkMergeChunks();
    });

    it("allows a retried splitChunk when allowChunkOperations is false", () => {
        this.checkSplitChunkIdempotency();
    });

    it("allows a retried mergeChunks when allowChunkOperations is false", () => {
        this.checkMergeChunksIdempotency();
    });
});
