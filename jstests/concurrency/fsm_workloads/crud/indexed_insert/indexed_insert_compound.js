/**
 * indexed_insert_compound.js
 *
 * Inserts documents into an indexed collection and asserts that the documents
 * appear in both a collection scan and an index scan. The collection is indexed
 * with a compound index on three different fields.
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {
    $config as $baseConfig
} from "jstests/concurrency/fsm_workloads/crud/indexed_insert/indexed_insert_base.js";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    $config.states.init = function init(db, collName) {
        $super.states.init.apply(this, arguments);
    };

    $config.data.getDoc = function getDoc() {
        return {
            indexed_insert_compound_x: this.tid & 0x0f,  // lowest 4 bits
            indexed_insert_compound_y: this.tid >> 4,    // high bits
            indexed_insert_compound_z: String.fromCharCode(33 + this.tid)
        };
    };

    $config.data.getIndexName = function getIndexName() {
        return 'indexed_insert_compound_x_1_' +
            'indexed_insert_compound_y_1_' +
            'indexed_insert_compound_z_1';
    };

    $config.data.getIndexSpec = function getIndexSpec() {
        return {
            indexed_insert_compound_x: 1,
            indexed_insert_compound_y: 1,
            indexed_insert_compound_z: 1
        };
    };

    $config.data.shardKey = $config.data.getIndexSpec();

    return $config;
});
