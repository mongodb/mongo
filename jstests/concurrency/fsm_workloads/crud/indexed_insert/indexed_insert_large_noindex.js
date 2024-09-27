/**
 * indexed_insert_large_noindex.js
 *
 * Executes the indexed_insert_large.js workload after dropping its index.
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {indexedNoindex} from "jstests/concurrency/fsm_workload_modifiers/indexed_noindex.js";
import {
    $config as $baseConfig
} from "jstests/concurrency/fsm_workloads/crud/indexed_insert/indexed_insert_large.js";

export const $config = extendWorkload($baseConfig, indexedNoindex);
