/**
 * update_inc_pipeline.js
 *
 * This is the same workload as update_inc.js, but substitutes a $mod-style update with a
 * pipeline-style one.
 * This workload implicitly assumes that its tid ranges are [0, $config.threadCount). This
 * isn't guaranteed to be true when they are run in parallel with other workloads. Therefore
 * it can't be run in concurrency simultaneous suites.
 * @tags: [incompatible_with_concurrency_simultaneous]
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/query/update/update_inc.js";

export const $config = extendWorkload($baseConfig, function($config, $super) {
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
