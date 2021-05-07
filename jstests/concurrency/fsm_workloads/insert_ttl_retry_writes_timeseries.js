'use strict';

/**
 * insert_ttl_retry_writes_timeseries.js
 *
 * Runs insert_ttl_timeseries.js with retryable writes.
 *
 * @tags: [
 *   assumes_no_implicit_collection_creation_after_drop,
 *   does_not_support_stepdowns,
 *   requires_fcv_49,
 *   requires_timeseries,
 *   requires_replication,
 *   uses_ttl,
 * ]
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');             // for extendWorkload
load('jstests/concurrency/fsm_workloads/insert_ttl_timeseries.js');  // for $config

var $config = extendWorkload($config, function($config, $super) {
    $config.data.getCollectionName = function getCollectionName(collName) {
        return "insert_ttl_retry_writes_timeseries_" + collName;
    };

    $config.data.getCollection = function getCollectionName(db, collName) {
        return $config.data.session.getDatabase(db.getName())
            .getCollection($config.data.getCollectionName(collName));
    };

    $config.states.init = function init(db, collName) {
        $config.data.session = db.getMongo().startSession({retryWrites: true});

        $super.states.init.apply(this, arguments);
    };

    return $config;
});
