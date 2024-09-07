/**
 * update_upsert_multi_noindex.js
 *
 * Executes the update_upsert_multi.js workload after dropping all non-_id
 * indexes on the collection.
 *
 * @tags: [requires_non_retryable_writes]
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {dropAllIndexes} from "jstests/concurrency/fsm_workload_modifiers/drop_all_indexes.js";
import {
    $config as $baseConfig
} from "jstests/concurrency/fsm_workloads/query/update/update_upsert_multi.js";

export const $config = extendWorkload($baseConfig, dropAllIndexes);
