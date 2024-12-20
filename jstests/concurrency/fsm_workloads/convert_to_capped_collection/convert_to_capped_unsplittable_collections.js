/**
 * Extends the `convert_to_capped_collection.js` with different tags for it to be executed on
 * sharding suites
 *
 * TODO SERVER-86392: delete this file once `assumes_unsharded` tests are ran in sharding suites
 *
 * @tags: [
 *   requires_capped,
 *   requires_sharding,
 *   # convertToCapped requires a global lock and any background operations on the database causes
 *   # it to fail due to not finishing quickly enough.
 *   incompatible_with_concurrency_simultaneous,
 * ]
 */

import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {
    $config as $baseConfig
} from
    'jstests/concurrency/fsm_workloads/convert_to_capped_collection/convert_to_capped_collection.js';

export const $config = extendWorkload($baseConfig, function($config, $super) {
    return $config;
});
