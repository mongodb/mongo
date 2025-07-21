/**
 * Tests time-series concurrent arbitrary update commands with the upsert option.
 *
 * @tags: [
 *   requires_timeseries,
 *   requires_non_retryable_writes,
 *   featureFlagTimeseriesUpdatesSupport,
 * ]
 */

import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {
    $config as $baseConfig
} from 'jstests/concurrency/fsm_workloads/timeseries/timeseries_updates_and_inserts.js';

export const $config = extendWorkload($baseConfig, function($config, $super) {
    $config.data.dateTime = new ISODate();

    // Update 'readingNo' for each sensor.
    $config.states.updateMany = function updateMany(db, collName) {
        const readingNo = Random.randInt(this.nTotalReadings);
        retryOnRetryableError(() => {
            assert.commandWorked(db.runCommand({
                update: collName,
                updates: [{
                    q: {readingNo: readingNo},
                    u: {$inc: {updatedMany: 1}},
                    multi: true,
                }]
            }));
        }, 100, undefined, TestData.runningWithBalancer ? [ErrorCodes.QueryPlanKilled] : []);
    };
    // Update one measurement for a random sensor. If no match is found, upsert one measurement for
    // that sensor.
    $config.states.updateOne = function updateOne(db, collName) {
        const sensorId = Random.randInt(this.nSensors);
        assert.commandWorked(db.runCommand({
            update: collName,
            updates: [{
                q: {sensorId: sensorId},
                u: {$set: {ts: this.dateTime, sensorId: sensorId, updatedOne: 1}},
                multi: false,
                upsert: true,
            }]
        }));
    };
    // Upsert a new reading for every sensor.
    $config.states.upsert = function upsert(db, collName) {
        const readingNo = this.startReadingNo++;
        for (let sensorId = 0; sensorId < this.nSensors; ++sensorId) {
            assert.commandWorked(db.runCommand({
                update: collName,
                updates: [{
                    q: {nonExistentField: 1},
                    u: {
                        _id: `${this.tid}${this.idCounter++}`,
                        sensorId: sensorId,
                        readingNo: readingNo,
                        ts: new ISODate(),
                    },
                    upsert: true,
                }]
            }));
        }
    };

    $config.transitions = {
        init: {updateMany: 0.25, upsert: 0.75},
        updateMany: {updateMany: 0.4, updateOne: 0.2, upsert: 0.4},
        updateOne: {updateMany: 0.4, updateOne: 0.2, upsert: 0.4},
        upsert: {updateMany: 0.4, updateOne: 0.2, upsert: 0.4}
    };

    return $config;
});
