'use strict';

/**
 * Runs insert, update, delete and findAndModify commands against a sharded collection inside
 * single-shard and cross-shard internal transactions using all the available client session
 * settings. Only runs on sharded clusters.
 *
 * @tags: [
 *  requires_fcv_60,
 *  requires_sharding,
 *  uses_transactions,
 *  antithesis_incompatible,
 *  # The default linearizable readConcern timeout is too low and may cause tests to fail.
 *  does_not_support_config_fuzzer,
 * ]
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');
load('jstests/concurrency/fsm_workloads/random_moveChunk_base.js');
load('jstests/concurrency/fsm_workloads/internal_transactions_unsharded.js');
load('jstests/libs/fail_point_util.js');

var $config = extendWorkload($config, function($config, $super) {
    $config.data.getQueryForDocument = function getQueryForDocument(doc) {
        // The query for a write command against a sharded collection must contain the shard key.
        const query = $super.data.getQueryForDocument.apply(this, arguments);
        query[this.defaultShardKeyField] = doc[this.defaultShardKeyField];
        return query;
    };

    $config.data.generateRandomDocument = function generateRandomDocument(
        tid, {partition, isLowerChunkDoc, isUpperChunkDoc} = {}) {
        const doc = $super.data.generateRandomDocument.apply(this, arguments);
        if (partition === undefined) {
            partition = this.partition;
        }
        assert.neq(partition, null);
        if (isLowerChunkDoc) {
            doc[this.defaultShardKeyField] =
                this.generateRandomInt(partition.lower, partition.mid - 1);
        } else if (isUpperChunkDoc) {
            doc[this.defaultShardKeyField] =
                this.generateRandomInt(partition.mid, partition.upper - 1);
        } else {
            doc[this.defaultShardKeyField] =
                this.generateRandomInt(partition.lower, partition.upper - 1);
        }
        return doc;
    };

    $config.data.insertInitialDocuments = function insertInitialDocuments(db, collName, tid) {
        const ns = db.getName() + "." + collName;
        const partition = this.makePartition(ns, tid, this.partitionSize);
        let bulk = db.getCollection(collName).initializeUnorderedBulkOp();
        for (let i = 0; i < this.partitionSize; ++i) {
            const doc = this.generateRandomDocument(tid, {partition});
            bulk.insert(doc);
        }
        assert.commandWorked(bulk.execute());
    };

    /**
     * Creates chunks for the collection that the commands in this workload runs against.
     */
    $config.setup = function setup(db, collName, cluster) {
        const ns = db.getName() + "." + collName;

        // Move the initial chunk to shard0.
        const shards = Object.keys(cluster.getSerializedCluster().shards);
        ChunkHelper.moveChunk(
            db,
            collName,
            [{[this.defaultShardKeyField]: MinKey}, {[this.defaultShardKeyField]: MaxKey}],
            shards[0]);

        for (let tid = 0; tid < this.threadCount; ++tid) {
            const partition = this.makePartition(ns, tid, this.partitionSize);

            // Create two chunks for the partition assigned to this thread:
            // [partition.lower, partition.mid] and [partition.mid, partition.upper]
            if (!partition.isLowChunk) {
                // The lower bound for a low chunk partition is minKey, so a split is not necessary.
                assert.commandWorked(db.adminCommand(
                    {split: ns, middle: {[this.defaultShardKeyField]: partition.lower}}));
            }
            assert.commandWorked(
                db.adminCommand({split: ns, middle: {[this.defaultShardKeyField]: partition.mid}}));

            // Move one of the two chunks assigned to this thread to one of the other shards so that
            // about half of the internal transactions run on this thread are cross-shard
            // transactions.
            ChunkHelper.moveChunk(
                db,
                collName,
                [
                    {[this.defaultShardKeyField]: partition.isLowChunk ? MinKey : partition.lower},
                    {[this.defaultShardKeyField]: partition.mid}
                ],
                shards[this.generateRandomInt(1, shards.length - 1)]);
        }

        db.printShardingStatus();

        if (this.insertInitialDocsOnSetUp) {
            // There isn't a way to determine what the thread ids are in setup phase so just assume
            // that they are [0, 1, ..., this.threadCount-1].
            for (let tid = 0; tid < this.threadCount; ++tid) {
                this.insertInitialDocuments(db, collName, tid);
            }
        }

        this.overrideInternalTransactionsReapThreshold(cluster);
        if (this.lowerTransactionLifetimeLimitSeconds) {
            this.overrideTransactionLifetimeLimit(cluster);
        }
    };

    return $config;
});
