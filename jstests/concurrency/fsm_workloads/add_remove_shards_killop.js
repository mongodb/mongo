/**
 * Tests that the cluster parameter "shardedClusterCardinalityForDirectConns" has the correct value
 * after the addShard, removeShard and killOp commands ran concurrently.
 *
 * This workload can't run in antithesis suites since it uses connCache.
 * @tags: [
 *  requires_sharding,
 *  requires_non_retryable_writes,
 *  catches_command_failures,
 *  uses_curop_agg_stage,
 *  antithesis_incompatible,
 *  featureFlagRSEndpointClusterCardinalityParameter,
 * ]
 */

import "jstests/libs/override_methods/retry_on_killed_session.js";
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {
    $config as $baseConfig
} from
    "jstests/concurrency/fsm_workloads_add_remove_shards/clusterwide_ops_with_add_remove_shards.js";
import {RetryableWritesUtil} from "jstests/libs/retryable_writes_util.js";
import {
    checkClusterParameter,
    interruptConfigsvrAddShard,
    interruptConfigsvrRemoveShard
} from "jstests/sharding/libs/cluster_cardinality_parameter_util.js";

// By default retry_on_killed_session.js will only retry known retryable operations like reads and
// retryable writes, but the addShard and removeShard commands in this test may be interrupted and
// are safe to retry so opt into always retrying killed operations.
TestData.alwaysRetryOnKillSessionErrors = true;

export const $config = extendWorkload($baseConfig, function($config, $super) {
    const originalInit = $config.states.init;
    const originalAddShard = $config.states.addShard;
    const originalRemoveShard = $config.states.removeShard;

    $config.states = {};

    $config.states.init = function(db, collName, connCache) {
        originalInit.call(this, db, collName);
    };

    $config.states.addShard = function(db, collName, connCache) {
        assert.soon(() => {
            try {
                originalAddShard.call(this, db, collName);
                return true;
            } catch (e) {
                if (e.code == ErrorCodes.ConflictingOperationInProgress) {
                    print("Skip retrying addShard after getting an error " + tojsononeline(e));
                    return true;
                }
                if (e.code == ErrorCodes.WriteConflict ||
                    e.code == ErrorCodes.FailedToSatisfyReadPreference) {
                    // TODO (SERVER-82909): Investigate why addShard can fail with WriteConflict.
                    // TODO (SERVER-82901): Investigate why addShard can fail with
                    // FailedToSatisfyReadPreference when it gets interrupted.
                    print("Retrying addShard after getting an error " + tojsononeline(e));
                    return false;
                }
                throw e;
            }
        });
    };

    $config.states.removeShard = function(db, collName, connCache) {
        originalRemoveShard.call(this, db, collName);
    };

    $config.states.interruptAddShard = function(db, collName, connCache) {
        connCache.config.forEach(conn => {
            interruptConfigsvrAddShard(conn);
        });
    };

    $config.states.interruptRemoveShard = function(db, collName, connCache) {
        connCache.config.forEach(conn => {
            interruptConfigsvrRemoveShard(conn);
        });
    };

    $config.teardown = function teardown(db, collName, cluster) {
        $super.teardown.apply(this, arguments);

        const shardDocs = db.getSiblingDB("config").getCollection("shards").find().toArray();
        jsTest.log("Checking the cluster parameter " +
                   tojsononeline({numShards: shardDocs.length, shardDocs}));
        const hasTwoOrMoreShards = shardDocs.length >= 2;

        cluster.getReplicaSets().forEach(rst => {
            while (true) {
                try {
                    checkClusterParameter(rst, hasTwoOrMoreShards)
                    return;
                } catch (e) {
                    if (RetryableWritesUtil.isRetryableCode(e.code)) {
                        print("Retry cluster parameter check after error: " + tojson(e));
                        continue;
                    }
                    throw e;
                }
            }
        });
    };

    const transitionProbabilities = {
        addShard: 0.25,
        removeShard: 0.25,
        interruptAddShard: 0.25,
        interruptRemoveShard: 0.25,
    };

    $config.transitions = {
        init: transitionProbabilities,
        addShard: transitionProbabilities,
        removeShard: transitionProbabilities,
        interruptAddShard: transitionProbabilities,
        interruptRemoveShard: transitionProbabilities,
    };

    $config.threadCount = 5;
    $config.iterations = 100;
    $config.passConnectionCache = true;

    return $config;
});
