/**
 * update_replace_noindex.js
 *
 * Executes the update_replace.js workload after dropping all non-_id indexes
 * on the collection.
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/update_replace.js";
load('jstests/concurrency/fsm_workload_modifiers/drop_all_indexes.js');  // for dropAllIndexes

export const $config = extendWorkload($baseConfig, dropAllIndexes);
