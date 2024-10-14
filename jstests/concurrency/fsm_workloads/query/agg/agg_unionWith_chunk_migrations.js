/**
 * agg_unionWith_chunk_migrations.js
 *
 * This tests exercises aggregations using $unionWith on a collection during chunk migrations. The
 * $unionWith stage is expected to internally retry the sub-queries during a chunk migration to
 * complete seamlessly.
 *
 * $config.data.collWithMigrations: collection to run chunk migrations against (default is the
 * collection of the aggregation itself).
 * $config.state.aggregate: function to execute the aggregation.
 *
 * @tags: [
 *  requires_sharding,
 *  assumes_balancer_off,
 *  requires_non_retryable_writes,
 *  requires_getmore,
 *  # The $unionWith stage cannot be run within a transaction. Therefore the original aggregation
 *  # and subsequent getMores will be run outside a transaction.
 *  uses_getmore_outside_of_transaction,
 * ]
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {
    $config as $baseConfig
} from "jstests/concurrency/fsm_workloads/query/agg/agg_with_chunk_migrations.js";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    $config.data.collWithMigrations = "union_ns";

    $config.states.aggregate = function aggregate(db, collName, connCache) {
        const res = db[collName].aggregate([{$unionWith: "union_ns"}]);
        assert.eq(this.numDocs * 2, res.itcount());
    };

    return $config;
});
