/**
 * update_inc_capped.js
 *
 * Executes the update_inc.js workload on a capped collection.
 *
 * This workload implicitly assumes that its tid ranges are [0, $config.threadCount). This
 * isn't guaranteed to be true when they are run in parallel with other workloads. Therefore
 * it can't be run in concurrency simultaneous suites.
 * @tags: [requires_capped, incompatible_with_concurrency_simultaneous]
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {makeCapped} from "jstests/concurrency/fsm_workload_modifiers/make_capped.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/query/update/update_inc.js";

export const $config = extendWorkload($baseConfig, makeCapped);
