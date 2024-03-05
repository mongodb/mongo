/**
 * Extends the `convert_to_capped_collection.js` with different tags for it to be executed on
 * sharding suites
 *
 * TODO SERVER-86392: delete this file once `assumes_unsharded` tests are ran in sharding suites
 *
 * @tags: [
 *   requires_capped,
 *   requires_sharding,
 *   featureFlagConvertToCappedCoordinator,
 *   # convertToCapped requires a global lock and any background operations on the database causes
 *   # it to fail due to not finishing quickly enough.
 *   incompatible_with_concurrency_simultaneous,
 *   # SERVER-85772 enable testing with balancer once convertToCapped supported on arbitrary shards
 *   assumes_balancer_off,
 * ]
 */

import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {
    $config as $baseConfig
} from 'jstests/concurrency/fsm_workloads/convert_to_capped_collection.js';

export const $config = extendWorkload($baseConfig, function($config, $super) {
    return $config;
});
