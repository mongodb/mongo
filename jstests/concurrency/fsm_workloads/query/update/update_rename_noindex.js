/**
 * update_rename_noindex.js
 *
 * Executes the update_rename.js workload after dropping all non-_id indexes on
 * the collection.
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {dropAllIndexes} from "jstests/concurrency/fsm_workload_modifiers/drop_all_indexes.js";
import {
    $config as $baseConfig
} from "jstests/concurrency/fsm_workloads/query/update/update_rename.js";

export const $config = extendWorkload($baseConfig, dropAllIndexes);
