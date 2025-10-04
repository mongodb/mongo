/**
 * indexed_insert_long_fieldname.js
 *
 * Inserts multiple documents into an indexed collection. Asserts that all
 * documents appear in both a collection scan and an index scan. The indexed
 * field name is a long string.
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/crud/indexed_insert/indexed_insert_base.js";

export const $config = extendWorkload($baseConfig, function ($config, $super) {
    // The indexedField must be limited such that the namespace and indexedField does not
    // exceed 128 characters. The namespace defaults to // "test<i>_fsmdb<j>.fsmcoll<k>",
    // where i, j & k are increasing integers for each test, workload and thread.
    // See https://docs.mongodb.com/manual/reference/limits/#Index-Name-Length
    let length = 90;
    let prefix = "indexed_insert_long_fieldname_";
    $config.data.indexedField = prefix + "x".repeat(length - prefix.length);
    $config.data.shardKey = {};
    $config.data.shardKey[$config.data.indexedField] = 1;

    return $config;
});
