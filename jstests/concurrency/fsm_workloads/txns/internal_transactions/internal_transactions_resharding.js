/**
 * Runs insert, update, delete and findAndModify commands against a sharded collection inside
 * single-shard and cross-shard internal transactions using all client session configurations, and
 * occasionally reshards the collection. Only runs on sharded clusters.
 *
 * @tags: [
 *  requires_fcv_60,
 *  requires_sharding,
 *  uses_transactions,
 *  antithesis_incompatible,
 *  assumes_stable_shard_list,
 * ]
 */
import {interruptedQueryErrors} from "jstests/concurrency/fsm_libs/assert.js";
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {executeReshardCollection} from "jstests/concurrency/fsm_libs/reshard_collection_util.js";
import {
    $config as $baseConfig
} from
    "jstests/concurrency/fsm_workloads/txns/internal_transactions/internal_transactions_sharded.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    const customShardKeyFieldName = "customShardKey";
    $config.data.shardKeys = [];
    $config.data.currentShardKeyIndex = -1;
    $config.data.reshardingCount = 0;

    $config.data.getQueryForDocument = function getQueryForDocument(doc) {
        // The query for a write command against a sharded collection must contain the shard key.
        const query = $super.data.getQueryForDocument.apply(this, arguments);
        query[customShardKeyFieldName] = doc[customShardKeyFieldName];
        return query;
    };

    $config.data.generateRandomDocument = function generateRandomDocument(tid, {partition} = {}) {
        const doc = $super.data.generateRandomDocument.apply(this, arguments);
        if (partition === undefined) {
            partition = this.partition;
        }
        assert.neq(partition, null);
        doc[customShardKeyFieldName] = this.generateRandomInt(partition.lower, partition.upper - 1);
        return doc;
    };

    $config.data.isAcceptableAggregateCmdError = function isAcceptableAggregateCmdError(res) {
        // The aggregate command is expected to involve running getMore commands which are not
        // retryable after a collection rename (done by resharding).
        return $baseConfig.data.isAcceptableAggregateCmdError(res) ||
            (res && (interruptedQueryErrors.includes(res.code)));
    };

    $config.data.isAcceptableRetryError = function isAcceptableRetryError(res) {
        // This workload does in-place resharding so a retry that is sent
        // reshardingMinimumOperationDurationMillis after resharding completes is expected to fail
        // with IncompleteTransactionHistory.
        return (res.code == ErrorCodes.IncompleteTransactionHistory) &&
            res.errmsg.includes("Incomplete history detected for transaction");
    };

    $config.states.init = function init(db, collName, connCache) {
        $super.states.init.apply(this, arguments);
        this.shardKeys.push({[this.defaultShardKeyField]: 1});
        this.shardKeys.push({[customShardKeyFieldName]: 1});
        this.currentShardKeyIndex = 0;
        this._allowSameKeyResharding =
            FeatureFlagUtil.isPresentAndEnabled(db.getMongo(), 'ReshardingImprovements');
    };

    $config.states.reshardCollection = function reshardCollection(db, collName, connCache) {
        executeReshardCollection(this, db, collName, connCache, false /*sameKeyResharding*/);
    };

    $config.states.reshardCollectionSameKey = function reshardCollectionSameKey(
        db, collName, connCache) {
        executeReshardCollection(this, db, collName, connCache, this._allowSameKeyResharding);
    };

    $config.transitions = {
        init: {
            internalTransactionForInsert: 0.25,
            internalTransactionForUpdate: 0.25,
            internalTransactionForDelete: 0.25,
            internalTransactionForFindAndModify: 0.25,
        },
        reshardCollection: {
            internalTransactionForInsert: 0.2,
            internalTransactionForUpdate: 0.2,
            internalTransactionForDelete: 0.2,
            internalTransactionForFindAndModify: 0.2,
            verifyDocuments: 0.2
        },
        reshardCollectionSameKey: {
            internalTransactionForInsert: 0.2,
            internalTransactionForUpdate: 0.2,
            internalTransactionForDelete: 0.2,
            internalTransactionForFindAndModify: 0.2,
            verifyDocuments: 0.2
        },
        internalTransactionForInsert: {
            reshardCollection: 0.1,
            reshardCollectionSameKey: 0.1,
            internalTransactionForInsert: 0.15,
            internalTransactionForUpdate: 0.15,
            internalTransactionForDelete: 0.15,
            internalTransactionForFindAndModify: 0.15,
            verifyDocuments: 0.2
        },
        internalTransactionForUpdate: {
            reshardCollection: 0.1,
            reshardCollectionSameKey: 0.1,
            internalTransactionForInsert: 0.15,
            internalTransactionForUpdate: 0.15,
            internalTransactionForDelete: 0.15,
            internalTransactionForFindAndModify: 0.15,
            verifyDocuments: 0.2
        },
        internalTransactionForDelete: {
            reshardCollection: 0.1,
            reshardCollectionSameKey: 0.1,
            internalTransactionForInsert: 0.15,
            internalTransactionForUpdate: 0.15,
            internalTransactionForDelete: 0.15,
            internalTransactionForFindAndModify: 0.15,
            verifyDocuments: 0.2
        },
        internalTransactionForFindAndModify: {
            reshardCollection: 0.1,
            reshardCollectionSameKey: 0.1,
            internalTransactionForInsert: 0.15,
            internalTransactionForUpdate: 0.15,
            internalTransactionForDelete: 0.15,
            internalTransactionForFindAndModify: 0.15,
            verifyDocuments: 0.2
        },
        verifyDocuments: {
            reshardCollection: 0.1,
            reshardCollectionSameKey: 0.1,
            internalTransactionForInsert: 0.2,
            internalTransactionForUpdate: 0.2,
            internalTransactionForDelete: 0.2,
            internalTransactionForFindAndModify: 0.2,
        }
    };

    return $config;
});
