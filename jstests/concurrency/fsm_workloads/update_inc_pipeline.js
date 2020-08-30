'use strict';

/**
 * update_inc_pipeline.js
 *
 * This is the same workload as update_inc.js, but substitutes a $mod-style update with a
 * pipeline-style one.
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');  // for extendWorkload
load('jstests/concurrency/fsm_workloads/update_inc.js');  // for $config

var $config = extendWorkload($config, function($config, $super) {
    $config.data.getUpdateArgument = function getUpdateArgument(fieldName) {
        return [{$set: {[fieldName]: {$add: ["$" + fieldName, 1]}}}];
    };

    $config.data.update_inc = "update_inc_pipeline";

    $config.setup = function(db, collName, cluster) {
        // Add 'largeStr' to the documents in order to make pipeline-based updates generate delta
        // oplog entries.
        var doc = {_id: this.id, largeStr: '*'.repeat(128)};

        // Pre-populate the fields we need to avoid size change for capped collections.
        for (var i = 0; i < this.threadCount; ++i) {
            doc['t' + i] = 0;
        }
        assert.commandWorked(db[collName].insert(doc));
    };

    return $config;
});
