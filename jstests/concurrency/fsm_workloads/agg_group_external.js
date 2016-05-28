'use strict';

/**
 * agg_group_external.js
 *
 * Runs an aggregation with a $group.
 *
 * The data passed to the $group is greater than 100MB, which should force
 * disk to be used.
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');         // for extendWorkload
load('jstests/concurrency/fsm_workloads/agg_base.js');           // for $config
load('jstests/concurrency/fsm_workload_helpers/drop_utils.js');  // for dropCollections

var $config = extendWorkload($config, function($config, $super) {

    // use enough docs to exceed 100MB, the in-memory limit for $sort and $group
    $config.data.numDocs = 24 * 1000;
    var MB = 1024 * 1024;  // bytes
    assertAlways.lte(100 * MB, $config.data.numDocs * $config.data.docSize);

    // assume no other workload will manipulate collections with this prefix
    $config.data.getOutputCollPrefix = function getOutputCollPrefix(collName) {
        return collName + '_out_agg_group_external_';
    };

    $config.states.query = function query(db, collName) {
        var otherCollName = this.getOutputCollPrefix(collName) + this.tid;
        var cursor = db[collName].aggregate(
            [{$group: {_id: '$randInt', count: {$sum: 1}}}, {$out: otherCollName}],
            {allowDiskUse: true});
        assertAlways.eq(0, cursor.itcount());
        assertWhenOwnColl(function() {
            // sum the .count fields in the output coll
            var sum = db[otherCollName]
                          .aggregate([{$group: {_id: null, totalCount: {$sum: '$count'}}}])
                          .toArray()[0]
                          .totalCount;
            assertWhenOwnColl.eq(this.numDocs, sum);
        }.bind(this));
    };

    $config.teardown = function teardown(db, collName, cluster) {
        $super.teardown.apply(this, arguments);

        // drop all collections with this workload's assumed-to-be-unique prefix
        // NOTE: assumes the prefix contains no special regex chars
        dropCollections(db, new RegExp('^' + this.getOutputCollPrefix(collName)));
    };

    return $config;
});
