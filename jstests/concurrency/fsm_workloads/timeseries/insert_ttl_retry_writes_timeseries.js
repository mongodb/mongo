/**
 * insert_ttl_retry_writes_timeseries.js
 *
 * Runs insert_ttl_timeseries.js with retryable writes.
 *
 * @tags: [
 *   assumes_no_implicit_collection_creation_after_drop,
 *   does_not_support_stepdowns,
 *   requires_replication,
 *   requires_timeseries,
 *   uses_ttl,
 * ]
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {
    $config as $baseConfig
} from "jstests/concurrency/fsm_workloads/timeseries/insert_ttl_timeseries.js";

export const $config = extendWorkload($baseConfig, function($config, $super) {
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
