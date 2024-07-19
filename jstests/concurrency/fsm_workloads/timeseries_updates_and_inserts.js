/**
 * Tests time-series arbirary updates and inserts can run concurrently.
 *
 * @tags: [
 *   requires_timeseries,
 *   requires_non_retryable_writes,
 *   featureFlagTimeseriesUpdatesSupport,
 * ]
 */

import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";

export const $config = (function() {
    const data = {
        nReadingsPerSensor: 100,
        nSensors: 100,
        // 100 to start + 5 threads * 100 each
        nTotalReadings: 100 + 5 * 100
    };
    const states = {
        init: function init(db, collName) {
            this.idCounter = 0;
            // Reading at which this thread should start inserting. The starting point begins after
            // the seed data and is based on the thread id is to ensure uniqueness across inserted
            // values.
            this.startReadingNo = this.nReadingsPerSensor + this.tid * this.nReadingsPerSensor;
        },
        updateMany: function updateMany(db, collName) {
            const readingNo = Random.randInt(this.nTotalReadings);
            let additionalCodesToRetry = [ErrorCodes.NoProgressMade];
            if (TestData.runningWithBalancer) {
                additionalCodesToRetry.push(ErrorCodes.QueryPlanKilled);
            }

            retryOnRetryableError(() => {
                assert.commandWorked(
                    db[collName].updateMany({readingNo: readingNo}, {$inc: {updated: 1}}));
            }, 100, undefined, additionalCodesToRetry);
        },
        updateOne: function updateOne(db, collName) {
            const sensorId = Random.randInt(this.nSensors);
            assert.commandWorked(
                db[collName].updateOne({sensorId: sensorId}, {$inc: {updated: 1}}));
        },
        insert: function insert(db, collName) {
            // Insert a new reading for every sensor.
            const readingNo = this.startReadingNo++;
            let bulk = db[collName].initializeUnorderedBulkOp();
            for (let sensorId = 0; sensorId < this.nSensors; ++sensorId) {
                bulk.insert({
                    _id: `${this.tid}${this.idCounter++}`,
                    sensorId: sensorId,
                    readingNo: readingNo,
                    ts: new ISODate()
                });
            }

            try {
                bulk.execute();
            } catch (e) {
                // TODO SERVER-85548 Remove this whole catch block.
                if (e.isNotSupported) {
                    throw e;
                }
                TimeseriesTest.assertInsertWorked(e);
            }
        }
    };

    const transitions = {
        init: {updateMany: 0.25, insert: 0.75},
        updateMany: {updateMany: 0.4, updateOne: 0.2, insert: 0.4},
        updateOne: {updateMany: 0.4, updateOne: 0.2, insert: 0.4},
        insert: {updateMany: 0.4, updateOne: 0.2, insert: 0.4}
    };

    function setup(db, collName, cluster) {
        // Lower the following parameter to force more yields.
        cluster.executeOnMongodNodes(function lowerYieldParams(db) {
            assert.commandWorked(
                db.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 10}));
        });

        db[collName].drop();
        db.createCollection(
            collName, {timeseries: {timeField: "ts", metaField: "sensorId", granularity: "hours"}});

        let bulk = db[collName].initializeUnorderedBulkOp();
        let idCounter = 0;
        for (let sensorId = 0; sensorId < data.nSensors; ++sensorId) {
            for (let i = 0; i < data.nReadingsPerSensor; ++i) {
                bulk.insert(
                    {_id: idCounter++, sensorId: sensorId, readingNo: i, ts: new ISODate()});
            }
        }
        bulk.execute();
    }

    function teardown(db, collName, cluster) {
        // Reset the yield parameter.
        cluster.executeOnMongodNodes(function lowerYieldParams(db) {
            assert.commandWorked(
                db.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 1000}));
        });
    }

    return {
        threadCount: 5,
        iterations: 50,
        data: data,
        states: states,
        transitions: transitions,
        setup: setup,
        teardown: teardown,
    };
})();
