'use strict';

/**
 * agg_match.js
 *
 * Runs an aggregation with a $match that returns half the documents.
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');  // for extendWorkload
load('jstests/concurrency/fsm_workloads/agg_base.js');    // for $config

var $config = extendWorkload($config, function($config, $super) {

    $config.data.getOutCollName = function getOutCollName(collName) {
        return collName + '_out_agg_match';
    };

    $config.states.query = function query(db, collName) {
        // note that all threads output to the same collection
        var otherCollName = this.getOutCollName(collName);
        var cursor = db[collName].aggregate([{$match: {flag: true}}, {$out: otherCollName}]);
        assertAlways.eq(0, cursor.itcount(), 'cursor returned by $out should always be empty');
        // NOTE: This relies on the fast-path for .count() with no query being isolated.
        // NOTE: There's a bug, SERVER-3645, where .count() is wrong on sharded collections, so we
        // blacklisted this test for sharded clusters.
        assertWhenOwnColl.eq(db[collName].count() / 2, db[otherCollName].count());
    };

    $config.setup = function setup(db, collName, cluster) {
        $super.setup.apply(this, arguments);

        // Create the collection to avoid a race in the initial aggregations. If the collection
        // doesn't exist, only one $out can create it, and the others will see their target has been
        // changed, and throw an error.
        assertWhenOwnColl.commandWorked(db.runCommand({create: this.getOutCollName(collName)}));
    };

    $config.teardown = function teardown(db, collName, cluster) {
        $super.teardown.apply(this, arguments);

        assertWhenOwnColl(db[this.getOutCollName(collName)].drop());
    };

    return $config;
});
