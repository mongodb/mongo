/**
 * indexed_insert_1char.js
 *
 * Inserts multiple documents into an indexed collection. Asserts that all
 * documents appear in both a collection scan and an index scan. The indexed
 * value is a 1-character string based on the thread's id.
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {
    $config as $baseConfig
} from "jstests/concurrency/fsm_workloads/crud/indexed_insert/indexed_insert_base.js";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    $config.data.indexedField = 'indexed_insert_1char';
    $config.data.shardKey = {};
    $config.data.shardKey[$config.data.indexedField] = 1;

    $config.states.init = function init(db, collName) {
        $super.states.init.apply(this, arguments);

        this.indexedValue = String.fromCharCode(33 + this.tid);
    };

    return $config;
});
