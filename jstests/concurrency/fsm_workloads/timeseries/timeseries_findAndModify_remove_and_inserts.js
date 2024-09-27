/**
 * timeseries/timeseries_findAndModify_remove_and_inserts.js
 *
 * Extends timeseries_delete_and_inserts.js to test findAndModify with {remove: true}.
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
} from 'jstests/concurrency/fsm_workloads/query/timeseries/timeseries_deletes_and_inserts.js';

export const $config = extendWorkload($baseConfig, function($config, $super) {
    $config.data.logColl = "findAndModify_remove_and_inserts_log";
    $config.states.findAndRemove = function findAndRemove(db, collName) {
        // Delete a reading from each bucket. Include readings that will be inserted to have
        // coverage on overlapping inserts and deletes.
        const readingNo = Random.randInt(this.nTotalReadings);
        for (let sensorId = 0; sensorId < this.nSensors; ++sensorId) {
            let res = assert.commandWorked(db.runCommand({
                findAndModify: collName,
                query: {readingNo: readingNo, sensorId: sensorId},
                remove: true,
            }));
            if (res.lastErrorObject.n) {
                const errMsg = `Deleted measurement ${res.value} should match the query predicate ${
                    tojson({readingNo: readingNo, sensorId: sensorId})} and all fields`;
                assert(res.value.readingNo == readingNo, errMsg);
                assert(res.value.sensorId == sensorId, errMsg);
                assert(res.value._id != undefined, errMsg);
                assert(res.value.ts != undefined, errMsg);
            }
        }

        // Log what we did to a side collection for later validation.
        assert.commandWorked(db[this.logColl].insert({readingNo: readingNo, deleted: true}));
    };

    $config.transitions = {
        init: {findAndRemove: 0.25, insert: 0.75},
        findAndRemove: {findAndRemove: 0.4, deleteBucket: 0.2, insert: 0.4},
        deleteBucket: {findAndRemove: 0.4, deleteBucket: 0.2, insert: 0.4},
        insert: {findAndRemove: 0.4, deleteBucket: 0.2, insert: 0.4}
    };

    return $config;
});
