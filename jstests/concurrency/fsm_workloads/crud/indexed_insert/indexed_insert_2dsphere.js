/**
 * indexed_insert_2dsphere.js
 *
 * Inserts documents into an indexed collection and asserts that the documents
 * appear in both a collection scan and an index scan. The indexed value is a
 * legacy coordinate pair, indexed with a 2dsphere index.
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {
    $config as $baseConfig
} from "jstests/concurrency/fsm_workloads/crud/indexed_insert/indexed_insert_2d.js";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    $config.data.indexedField = 'indexed_insert_2dsphere';

    $config.data.getIndexSpec = function getIndexSpec() {
        var ixSpec = {};
        ixSpec[this.indexedField] = '2dsphere';
        return ixSpec;
    };

    return $config;
});
