/**
 * agg_group_external.js
 *
 * Runs an aggregation with a $group.
 *
 * The data passed to the $group is greater than 100MB, which should force
 * disk to be used.
 *
 * @tags: [
 *      # These workloads uses >100MB of data, which can overwhelm test hosts.
 *      requires_standalone,
 *      incompatible_with_concurrency_simultaneous,
 *      requires_getmore,
 *      uses_getmore_outside_of_transaction,
 * ]
 *
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/query/agg/agg_base.js";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    $config.setup = function setup(db, collName, cluster) {
        this.numDocs = 24 * 1000;
        $super.setup.apply(this, [db, collName, cluster]);
        // use enough docs to exceed 100MB, the in-memory limit for $sort and $group
        const MB = 1024 * 1024;
        // TODO SERVER-92452: Remove the if statement (but not the assert) once we fix the
        // WT_CACHE_FULL problem.
        if (!this.anyNodeIsEphemeral) {
            assert.lte(100 * MB, this.numDocs * this.docSize);
        }
    };

    // assume no other workload will manipulate collections with this prefix
    $config.data.getOutputCollPrefix = function getOutputCollPrefix(collName) {
        return collName + '_out_agg_group_external_';
    };

    $config.states.query = function query(db, collName) {
        var otherCollName = this.getOutputCollPrefix(collName) + this.tid;
        var cursor = db[collName].aggregate(
            [{$group: {_id: '$randInt', count: {$sum: 1}}}, {$out: otherCollName}],
            {allowDiskUse: true});
        assert.eq(0, cursor.itcount());
        // sum the .count fields in the output coll
        var sum = db[otherCollName]
                      .aggregate([{$group: {_id: null, totalCount: {$sum: '$count'}}}])
                      .toArray()[0]
                      .totalCount;
        assert.eq(this.numDocs, sum);
    };

    return $config;
});
