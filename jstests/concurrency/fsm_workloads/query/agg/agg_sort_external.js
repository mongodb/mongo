/**
 * agg_sort_external.js
 *
 * Runs an aggregation with a $match that returns half the documents followed
 * by a $sort on a field containing a random float.
 *
 * The data returned by the $match is greater than 100MB, which should force an external sort.
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
            // assert that *half* the docs exceed the in-memory limit, because the $match stage will
            // only pass half the docs in the collection on to the $sort stage.
            assert.lte(100 * MB, this.numDocs * this.docSize / 2);
        }
    };

    $config.data.getOutputCollPrefix = function getOutputCollPrefix(collName) {
        return collName + '_out_agg_sort_external_';
    };

    $config.states.query = function query(db, collName) {
        var otherCollName = this.getOutputCollPrefix(collName) + this.tid;
        var cursor = db[collName].aggregate(
            [{$match: {flag: true}}, {$sort: {rand: 1}}, {$out: otherCollName}],
            {allowDiskUse: true});
        assert.eq(0, cursor.itcount());
        assert.eq(db[collName].find().itcount() / 2, db[otherCollName].find().itcount());
    };

    return $config;
});
