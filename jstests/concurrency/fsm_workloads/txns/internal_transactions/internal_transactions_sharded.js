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
 *  assumes_stable_shard_list,
 * ]
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {parseConfig} from "jstests/concurrency/fsm_libs/parse_config.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/sharded_partitioned/crud_base_partitioned.js";
import {extendWithInternalTransactionsUnsharded} from "jstests/concurrency/fsm_workloads/txns/internal_transactions/internal_transactions_unsharded.js";

const parsedBaseConfig = parseConfig($baseConfig);
const $extendedBaseConfig = extendWithInternalTransactionsUnsharded(
    Object.extend({}, parsedBaseConfig, true),
    parsedBaseConfig,
);

export const $config = extendWorkload($extendedBaseConfig, function ($config, $super) {
    $config.data.getQueryForDocument = function getQueryForDocument(doc) {
        // The query for a write command against a sharded collection must contain the shard key.
        const query = $super.data.getQueryForDocument.apply(this, arguments);
        query[this.defaultShardKeyField] = doc[this.defaultShardKeyField];
        return query;
    };

    $config.data.generateRandomDocument = function generateRandomDocument(
        tid,
        {partition, isLowerChunkDoc, isUpperChunkDoc} = {},
    ) {
        const doc = $super.data.generateRandomDocument.apply(this, arguments);
        if (partition === undefined) {
            partition = this.partition;
        }
        assert.neq(partition, null);
        if (isLowerChunkDoc) {
            doc[this.defaultShardKeyField] = this.generateRandomInt(partition.lower, partition.mid - 1);
        } else if (isUpperChunkDoc) {
            doc[this.defaultShardKeyField] = this.generateRandomInt(partition.mid, partition.upper - 1);
        } else {
            doc[this.defaultShardKeyField] = this.generateRandomInt(partition.lower, partition.upper - 1);
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
