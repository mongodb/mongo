/**
 * Extends random_moveChunk_timeseries_arbitrary_updates.js workload with findAndModify updates.
 * Tests updates in the presence of concurrent insert and moveChunk commands.
 * @tags: [
 *  requires_sharding,
 *  resource_intensive,
 *  assumes_balancer_off,
 *  # Time-series findAndModify does not support retryable writes.
 *  requires_non_retryable_writes,
 *  does_not_support_transactions,
 *  featureFlagTimeseriesUpdatesSupport,
 * ]
 */

import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {
    $config as $baseConfig
} from 'jstests/concurrency/fsm_workloads/random_moveChunk_timeseries_arbitrary_updates.js';

const logCollection = "log_collection";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    // Perform arbitrary updates on metric fields of measurements.
    $config.states.arbitraryFindAndModifyUpdate = function(db, collName, connCache) {
        const fieldNameF = "f";
        const fieldNameTid = `tid${this.tid}`;
        const filterFieldName = `${fieldNameF}.${fieldNameTid}`;
        const filterFieldVal = Random.randInt($config.data.numMetaCount);
        const filter = {
            [filterFieldName]: {
                $gte: filterFieldVal,
            },
        };

        const res = assert.commandWorked(db.runCommand({
            findAndModify: collName,
            query: filter,
            new: true,
            update: {$inc: {"updateCount": 1}}
        }));

        if (res.lastErrorObject.n) {
            const errMsgRes =
                `Updated measurement ${tojson(res.value)} should match the query predicate ${
                    tojson(filter)}} and have all fields.`;
            assert(res.value != undefined, errMsgRes);
            assert(res.value[fieldNameF][fieldNameTid] >= filterFieldVal, errMsgRes);
            assert(res.value.updateCount >= 1, errMsgRes);
            assert(res.value._id != undefined, errMsgRes);
            assert(res.value.t != undefined, errMsgRes);
            // Log the number of updates performed.
            assert.commandWorked(db[logCollection].insert({"updateCount": 1}));
        }
    };

    $config.transitions = {
        init: {insert: 1},
        insert: {insert: 1, arbitraryFindAndModifyUpdate: 3, moveChunk: 1},
        arbitraryFindAndModifyUpdate: {insert: 1, arbitraryFindAndModifyUpdate: 3, moveChunk: 1},
        moveChunk: {insert: 1, arbitraryFindAndModifyUpdate: 1, moveChunk: 0},
    };

    return $config;
});
