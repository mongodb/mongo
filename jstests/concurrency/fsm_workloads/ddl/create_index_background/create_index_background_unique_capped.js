/**
 * create_index_background_unique_capped.js
 *
 * Creates multiple unique background indexes in parallel, on capped collections.
 *
 * @tags: [creates_background_indexes, requires_capped, assumes_balancer_off]
 */

import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {
    $config as $baseConfig
} from
    "jstests/concurrency/fsm_workloads/ddl/create_index_background/create_index_background_unique.js";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    $config.data.prefix = "create_index_background_unique_capped_";
    $config.data.getCollectionOptions = function() {
        // We create an 8MB capped collection, as it will comfortably fit the collection data
        // inserted by this test.
        const ONE_MB = 1024 * 1024;
        return {capped: true, size: 8 * ONE_MB};
    };

    return $config;
});
