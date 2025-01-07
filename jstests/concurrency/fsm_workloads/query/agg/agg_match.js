/**
 * agg_match.js
 *
 * Runs an aggregation with a $match that returns half the documents.
 * @tags: [
 *   # SERVER-33753, '.count() without a predicate can be wrong on sharded
 *   # collections'. This bug is problematic for these workloads because they assert on count()
 *   # values
 *   assumes_unsharded_collection,
 *   requires_getmore,
 *   uses_getmore_outside_of_transaction,
 * ]
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/query/agg/agg_base.js";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    $config.data.getOutCollName = function getOutCollName(collName) {
        return collName + '_out_agg_match';
    };

    $config.states.query = function query(db, collName) {
        // note that all threads output to the same collection
        var otherCollName = this.getOutCollName(collName);
        var cursor = db[collName].aggregate([{$match: {flag: true}}, {$out: otherCollName}]);
        assert.eq(0, cursor.itcount(), 'cursor returned by $out should always be empty');
        // NOTE: This relies on the fast-path for .count().
        // NOTE: There's a bug, SERVER-33753, where "fast" .count() is wrong on sharded
        // collections, so we denylisted this test for sharded clusters.
        assert.eq(db[collName].count() / 2, db[otherCollName].count());
    };

    $config.setup = function setup(db, collName, cluster) {
        $super.setup.apply(this, arguments);

        // Create the collection to avoid a race in the initial aggregations. If the collection
        // doesn't exist, only one $out can create it, and the others will see their target has been
        // changed, and throw an error.
        assert.commandWorked(db.runCommand({create: this.getOutCollName(collName)}));
    };

    return $config;
});
