'use strict';

/**
 * timeseries_deletes_and_inserts.js
 *
 * Inserts a bunch of seed data into a time-series collection and then issues a bunch of concurrent
 * partial and full bucket multi-deletes as well as inserts into the same buckets. These are all
 * designed to overlap in their targets, with each completed operation being logged to a side
 * collection. The validation checks that the data remaining in the collection is consistent with
 * the operations that were executed.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   featureFlagTimeseriesUpdatesDeletesSupport,
 *   __TEMPORARILY_DISABLED__,
 * ]
 */

var $config = (function() {
    const data = {
        logColl: "deletes_and_inserts_log",
        nReadingsPerSensor: 100,
        nSensors: 100,
        // 100 to start + 3 threads * 100 each
        nTotalReadings: 100 + 3 * 100
    };
    const states = {
        init: function init(db, collName) {
            this.idCounter = 0;
            // Reading at which this thread should start inserting. The starting point begins after
            // the seed data and is based on the thread id is to ensure uniqueness across inserted
            // values.
            this.startReadingNo = this.nReadingsPerSensor + this.tid * this.nReadingsPerSensor;
        },
        deleteMany: function deleteMany(db, collName) {
            // Delete a reading from each bucket. Include readings that will be inserted to have
            // coverage on overlapping inserts and deletes.
            const readingNo = Random.randInt(this.nTotalReadings);
            assert.commandWorked(db[collName].deleteMany({readingNo: readingNo}));

            // Log what we did to a side collection for later validation.
            assert.commandWorked(db[this.logColl].insert({readingNo: readingNo, deleted: true}));
        },
        deleteBucket: function deleteBucket(db, collName) {
            // Delete an entire bucket.
            const sensorId = Random.randInt(this.nSensors);
            assert.commandWorked(db[collName].deleteMany({sensorId: sensorId}));

            // Log what we did to a side collection for later validation.
            assert.commandWorked(db[this.logColl].insert({sensorId: sensorId, deleted: true}));
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
            bulk.execute();

            // Log what we did to a side collection for later validation.
            assert.commandWorked(db[this.logColl].insert({readingNo: readingNo, inserted: true}));
        }
    };

    var transitions = {
        init: {deleteMany: 0.25, insert: 0.75},
        deleteMany: {deleteMany: 0.4, deleteBucket: 0.2, insert: 0.4},
        deleteBucket: {deleteMany: 0.4, deleteBucket: 0.2, insert: 0.4},
        insert: {deleteMany: 0.4, deleteBucket: 0.2, insert: 0.4}
    };

    function setup(db, collName, cluster) {
        // Lower the following parameter to force more yields.
        cluster.executeOnMongodNodes(function lowerYieldParams(db) {
            assertAlways.commandWorked(
                db.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 10}));
        });

        db[collName].drop();
        db.createCollection(
            collName, {timeseries: {timeField: "ts", metaField: "sensorId", granularity: "hours"}});

        // Create a bunch of measurements for different sensors. We will try to create the data
        // in such a way that a multi-delete will try to target one or more measurement from
        // each bucket - this should induce some conflicts on writing to the bucket. 'readingNo'
        // will be our target.
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
            assertAlways.commandWorked(
                db.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 1000}));
        });

        const logColl = db[data.logColl];

        const deletedSensors = logColl.distinct("sensorId");
        const nSensorsRemaining = data.nSensors - deletedSensors.length;

        // Now validate the state of each reading. We will check all of the seed data and each
        // reading that we may have inserted.
        for (let readingNo = 0; readingNo < data.nTotalReadings; ++readingNo) {
            const wasDeleted = logColl.count({readingNo: readingNo, deleted: true}) > 0;
            const wasInserted = logColl.count({readingNo: readingNo, inserted: true}) > 0;

            const nReadings = db[collName].count({readingNo: readingNo});

            if (wasDeleted && !wasInserted) {
                // Easy case: this reading was deleted and never inserted - we expect 0 records.
                assertWhenOwnColl(nReadings == 0,
                                  `Expected all of the readings to be deleted: readingNo: ${
                                      readingNo}, nReadings: ${nReadings}`);
            } else if (wasInserted && !wasDeleted) {
                // This reading was inserted but not deleted. We should expect
                // readings for AT LEAST the number of remaining sensors. We may see more than this
                // if the insert happened after a sensor was deleted.
                assertWhenOwnColl(
                    nReadings >= nSensorsRemaining,
                    `Expected all of the remaining sensors' readings to be inserted: readingNo: ${
                        readingNo}, nReadings: ${nReadings}, nSensorsRemaining: ${
                        nSensorsRemaining}`);
            } else if (wasInserted && wasDeleted) {
                // This reading was both inserted and deleted. Since we don't know which order the
                // operations happened in, allow both cases through (but expect a consistent
                // result).
                assertWhenOwnColl(
                    nReadings == 0 || nReadings >= nSensorsRemaining,
                    `Expected all or none of the remaining sensors' readings to be deleted: readingNo: ${
                        readingNo}, nReadings: ${nReadings}, nSensorsRemaining: ${
                        nSensorsRemaining}`);
            } else {
                // This reading was not inserted or deleted. If it was a part of the seed data, make
                // sure that it still exists.
                if (readingNo < data.nReadingsPerSensor) {
                    assertWhenOwnColl(
                        nReadings == nSensorsRemaining,
                        `Expected none of the remaining sensors' readings to be deleted: readingNo: ${
                            readingNo}, nReadings: ${nReadings}, nSensorsRemaining: ${
                            nSensorsRemaining}`);
                }
            }
        }

        // Now make sure that any full-bucket deletions at least deleted all original records.
        for (const deletedSensor of deletedSensors) {
            const minReading = db[collName]
                                   .aggregate([
                                       {$match: {sensorId: deletedSensor}},
                                       {$group: {_id: null, min: {$min: "$readingNo"}}}
                                   ])
                                   .toArray();

            assertWhenOwnColl(
                minReading.length == 0 || minReading[0].min >= data.nReadingsPerSensor,
                `Expected all of the original readings to be deleted: sensorId: ${
                    deletedSensor.sensorId}, minReading: ${tojson(minReading)}`);
        }
    }

    return {
        threadCount: 3,
        iterations: 50,
        data: data,
        states: states,
        transitions: transitions,
        setup: setup,
        teardown: teardown,
    };
})();
