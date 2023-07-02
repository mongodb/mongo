/**
 * update_inc_capped.js
 *
 * Executes the update_inc.js workload on a capped collection.
 * @tags: [requires_capped]
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/update_inc.js";
load('jstests/concurrency/fsm_workload_modifiers/make_capped.js');  // for makeCapped

export const $config = extendWorkload($baseConfig, makeCapped);
