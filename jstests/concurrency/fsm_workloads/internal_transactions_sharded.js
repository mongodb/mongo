'use strict';

/**
 * Runs insert, update, delete and findAndModify commands against a sharded collection inside
 * single-shard and cross-shard internal transactions using all the available client session
 * settings. Only runs on sharded clusters.
 *
 * @tags: [
 *  requires_fcv_60,
 *  requires_sharding,
 *  uses_transactions
 * ]
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');
load('jstests/concurrency/fsm_workloads/random_moveChunk_base.js');
load('jstests/concurrency/fsm_workloads/internal_transactions_unsharded.js');
load('jstests/libs/fail_point_util.js');

var $config = extendWorkload($config, function($config, $super) {
    // This workload sets the 'storeFindAndModifyImagesInSideCollection' parameter to a random bool
    // during setup() and restores the original value during teardown().
    $config.data.originalStoreFindAndModifyImagesInSideCollection = {};

    $config.data.getQueryForDocument = function getQueryForDocument(collection, doc) {
        // The query for a write command against a sharded collection must contain the shard key.
        const shardKeyFieldName = this.shardKeyField[collection.getName()];
        return {_id: doc._id, tid: this.tid, [shardKeyFieldName]: doc[shardKeyFieldName]};
    };

    $config.data.generateRandomDocument = function generateRandomDocument(collection) {
        const shardKeyFieldName = this.shardKeyField[collection.getName()];
        return {
            _id: UUID(),
            tid: this.tid,
            [shardKeyFieldName]:
                this.generateRandomInt(this.partition.lower, this.partition.upper - 1),
            counter: 0
        };
    };

    /**
     * Creates chunks for the collection that the commands in this workload runs against.
     */
    $config.setup = function setup(db, collName, cluster) {
        const collection = db.getCollection(collName);
        const ns = collection.getFullName();

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

        const enableFindAndModifyImageCollection = this.generateRandomBool();
        cluster.executeOnMongodNodes((db) => {
            const res = assert.commandWorked(db.adminCommand({
                setParameter: 1,
                storeFindAndModifyImagesInSideCollection: enableFindAndModifyImageCollection
            }));
            this.originalStoreFindAndModifyImagesInSideCollection[db.getMongo().host] = res.was;
        });

        cluster.executeOnMongosNodes((db) => {
            configureFailPoint(db, "skipTransactionApiRetryCheckInHandleError");
        });
    };

    $config.teardown = function teardown(db, collName, cluster) {
        cluster.executeOnMongodNodes((db) => {
            assert.commandWorked(db.adminCommand({
                setParameter: 1,
                storeFindAndModifyImagesInSideCollection:
                    this.originalStoreFindAndModifyImagesInSideCollection[db.getMongo().host]
            }));
        });

        cluster.executeOnMongosNodes((db) => {
            configureFailPoint(db, "skipTransactionApiRetryCheckInHandleError", {}, "off");
        });
    };

    return $config;
});
