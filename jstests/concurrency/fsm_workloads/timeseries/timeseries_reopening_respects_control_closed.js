/**
 * Extends timeseries_crud_operations_respect_control_closed.js to focus on conflicting reopening
 * requests and direct writes to set the control.closed field to true.
 *
 * This test tests that concurrent user inserts, which may try to reopen buckets to insert into,
 * respect the control.closed:true field and do not write to closed buckets. Buckets that we attempt
 * to reopen but which we write directly to in between beginning our reopening request and actually
 * reopening them should fail to be reopened.
 *
 * @tags: [
 *  requires_timeseries,
 *  # Timeseries do not support multi-document transactions with inserts.
 *   does_not_support_transactions,
 * ]
 */

import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {
    $config as $baseConfig
} from
    'jstests/concurrency/fsm_workloads/timeseries/timeseries_crud_operations_respect_control_closed.js';

export const $config = extendWorkload($baseConfig, function($config, $super) {
    const standardTransition = {
        insertOrdered: 1,
        insertUnordered: 1,
        setControlClosedTrue: 1,
    };

    $config.transitions = {
        init: standardTransition,
        insertOrdered: standardTransition,
        insertUnordered: standardTransition,
        setControlClosedTrue: standardTransition,
    };

    return $config;
});
