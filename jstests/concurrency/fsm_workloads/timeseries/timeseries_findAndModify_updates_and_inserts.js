/**
 * timeseries_findAndModify_updates.js
 *
 * Extends timeseries/timeseries_updates_and_inserts.js to test findAndModify with updates.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   # Time-series findAndModify does not support retryable writes.
 *   requires_non_retryable_writes,
 *   featureFlagTimeseriesUpdatesSupport,
 * ]
 */

import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {
    $config as $baseConfig
} from 'jstests/concurrency/fsm_workloads/timeseries/timeseries_updates_and_inserts.js';

export const $config = extendWorkload($baseConfig, function($config, $super) {
    $config.states.findAndUpdateMany = function findAndUpdate(db, collName) {
        // Update a reading in each bucket.
        const readingNo = Random.randInt(this.nTotalReadings);
        for (let sensorId = 0; sensorId < this.nSensors; ++sensorId) {
            let res = assert.commandWorked(db.runCommand({
                findAndModify: collName,
                query: {readingNo: readingNo, sensorId: sensorId},
                new: true,
                update: {$inc: {updated: 1}}
            }));
            if (res.lastErrorObject.n) {
                const errMsg = `Updated measurement ${res.value} should match the query predicate ${
                    tojson({readingNo: readingNo, sensorId: sensorId})} and have all fields`;
                assert(res.value != undefined, errMsg);
                assert(res.value.readingNo == readingNo, errMsg);
                assert(res.value.sensorId == sensorId, errMsg);
                assert(res.value.updated >= 1, errMsg);
                assert(res.value._id != undefined, errMsg);
                assert(res.value.ts != undefined, errMsg);
            }
        }
    };
    $config.transitions = {
        init: {findAndUpdateMany: 0.25, insert: 0.75},
        findAndUpdateMany: {findAndUpdateMany: 0.5, insert: 0.5},
        insert: {findAndUpdateMany: 0.5, insert: 0.5}
    };

    return $config;
});
