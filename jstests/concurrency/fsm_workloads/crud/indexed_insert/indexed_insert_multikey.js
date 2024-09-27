/**
 * indexed_insert_multikey.js
 *
 * Inserts multiple documents into an indexed collection. Asserts that all
 * documents appear in both a collection scan and an index scan. The indexed
 * value is an array of numbers.
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {
    $config as $baseConfig
} from "jstests/concurrency/fsm_workloads/crud/indexed_insert/indexed_insert_base.js";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    $config.data.indexedField = 'indexed_insert_multikey';
    // Remove the shard key, since it cannot be a multikey index
    delete $config.data.shardKey;

    $config.states.init = function init(db, collName) {
        $super.states.init.apply(this, arguments);

        this.indexedValue = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9].map(function(n) {
            return this.tid * 10 + n;
        }.bind(this));
    };

    return $config;
});
