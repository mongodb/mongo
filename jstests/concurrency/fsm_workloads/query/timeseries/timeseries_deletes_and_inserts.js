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
 *   requires_non_retryable_writes,
 *   requires_fcv_70,
 * ]
 */

// TODO (SERVER-88275) a moveCollection can cause the original collection to be dropped and
// re-created with a different uuid, causing the aggregation to fail with QueryPlannedKilled
// when the mongos is fetching data from the shard using getMore(). Remove the helper and
// allowedErrorCodes from the entire test once this issue is fixed
function retryUntilWorked(query, readConcernIsObject = false) {
    let attempts = 0;
    let options = TestData.runningWithBalancer
        ? readConcernIsObject
            ? {"readConcern": {level: "majority"}}
            : {"readConcern": "majority"}
        : {};
    while (attempts < 3) {
        try {
            return query(options);
        } catch (e) {
            if (e.code == ErrorCodes.QueryPlanKilled && TestData.runningWithBalancer) {
                attempts++;
            } else {
                throw e;
            }
        }
    }
}

export const $config = (function () {
    const data = {
        logColl: "deletes_and_inserts_log",
        nReadingsPerSensor: 100,
        nSensors: 100,
        // 100 to start + 3 threads * 100 each
        nTotalReadings: 100 + 3 * 100,
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
                    ts: new ISODate(),
                });
            }
            bulk.execute();

            // Log what we did to a side collection for later validation.
            assert.commandWorked(db[this.logColl].insert({readingNo: readingNo, inserted: true}));
        },
    };

    let transitions = {
        init: {deleteMany: 0.25, insert: 0.75},
        deleteMany: {deleteMany: 0.4, deleteBucket: 0.2, insert: 0.4},
        deleteBucket: {deleteMany: 0.4, deleteBucket: 0.2, insert: 0.4},
        insert: {deleteMany: 0.4, deleteBucket: 0.2, insert: 0.4},
    };

    function setup(db, collName, cluster) {
        // Lower the following parameter to force more yields.
        cluster.executeOnMongodNodes(function lowerYieldParams(db) {
            assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 10}));
        });

        // Set the following parameter to avoid losing multi-updates and deletes,
        // i.e. all deletes that span multiple buckets.  This became necessary after SERVER-105874,
        // which added splitting to chunk migrations, and made it so that multi-updates such as
        // those coming from the deletes across readingNo can no longer guarantee complete success.
        // The flag here was added in SPM-3209 to protect against this.
        const preCheckResponse = assert.commandWorked(
            db.adminCommand({getClusterParameter: "pauseMigrationsDuringMultiUpdates"}),
        );
        let migrationsWerentPaused = !preCheckResponse.clusterParameters[0].enabled;
        if (migrationsWerentPaused) {
            assert.commandWorked(
                db.adminCommand({setClusterParameter: {pauseMigrationsDuringMultiUpdates: {enabled: true}}}),
            );
            cluster.executeOnMongosNodes((db) => {
                // Ensure all mongoses have refreshed cluster parameter after being set.
                assert.soon(() => {
                    const response = assert.commandWorked(
                        db.adminCommand({getClusterParameter: "pauseMigrationsDuringMultiUpdates"}),
                    );
                    return response.clusterParameters[0].enabled;
                });
            });
            assert.commandWorked(db[this.logColl].insert({migrationsNeedReset: true}));
        }

        db[collName].drop();
        db.createCollection(collName, {timeseries: {timeField: "ts", metaField: "sensorId", granularity: "hours"}});

        // Create a bunch of measurements for different sensors. We will try to create the data
        // in such a way that a multi-delete will try to target one or more measurement from
        // each bucket - this should induce some conflicts on writing to the bucket. 'readingNo'
        // will be our target.
        let bulk = db[collName].initializeUnorderedBulkOp();
        let idCounter = 0;
        for (let sensorId = 0; sensorId < data.nSensors; ++sensorId) {
            for (let i = 0; i < data.nReadingsPerSensor; ++i) {
                bulk.insert({_id: idCounter++, sensorId: sensorId, readingNo: i, ts: new ISODate()});
            }
        }
        bulk.execute();
    }

    function teardown(db, collName, cluster) {
        // Reset the yield parameter.
        cluster.executeOnMongodNodes(function lowerYieldParams(db) {
            assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 1000}));
        });

        const logColl = db[this.logColl];

        const deletedSensors = retryUntilWorked((options) => {
            return logColl.distinct("sensorId");
        });
        const nSensorsRemaining = data.nSensors - deletedSensors.length;

        // Now validate the state of each reading. We will check all of the seed data and each
        // reading that we may have inserted.
        for (let readingNo = 0; readingNo < data.nTotalReadings; ++readingNo) {
            const wasDeleted = retryUntilWorked((options) => {
                return logColl.count({readingNo: readingNo, deleted: true}, options) > 0;
            });
            const wasInserted = retryUntilWorked((options) => {
                return logColl.count({readingNo: readingNo, inserted: true}, options) > 0;
            });
            const nReadings = retryUntilWorked((options) => {
                return db[collName].count({readingNo: readingNo}, options);
            });

            if (wasDeleted && !wasInserted) {
                // Easy case: this reading was deleted and never inserted - we expect 0 records.
                assert(
                    nReadings == 0,
                    `Expected all of the readings to be deleted: readingNo: ${readingNo}, nReadings: ${nReadings}`,
                );
            } else if (wasInserted && !wasDeleted) {
                // This reading was inserted but not deleted. We should expect
                // readings for AT LEAST the number of remaining sensors. We may see more than this
                // if the insert happened after a sensor was deleted.
                assert(
                    nReadings >= nSensorsRemaining,
                    `Expected all of the remaining sensors' readings to be inserted: readingNo: ${
                        readingNo
                    }, nReadings: ${nReadings}, nSensorsRemaining: ${nSensorsRemaining}`,
                );
            } else if (wasInserted && wasDeleted) {
                // This reading was both inserted and deleted. Since the operations could happen
                // concurrently, any number of readings could exist in the collection in the end.
                // Skip checking anything for this case.
            } else {
                // This reading was not inserted or deleted. If it was a part of the seed data, make
                // sure that it still exists.
                if (readingNo < data.nReadingsPerSensor) {
                    assert(
                        nReadings == nSensorsRemaining,
                        `Expected none of the remaining sensors' readings to be deleted: readingNo: ${
                            readingNo
                        }, nReadings: ${nReadings}, nSensorsRemaining: ${nSensorsRemaining}`,
                    );
                }
            }
        }

        // Now make sure that any full-bucket deletions at least deleted all original records.
        for (const deletedSensor of deletedSensors) {
            const minReading = retryUntilWorked((options) => {
                return db[collName]
                    .aggregate(
                        [{$match: {sensorId: deletedSensor}}, {$group: {_id: null, min: {$min: "$readingNo"}}}],
                        options,
                    )
                    .toArray();
            }, true);

            assert(
                minReading.length == 0 || minReading[0].min >= data.nReadingsPerSensor,
                `Expected all of the original readings to be deleted: sensorId: ${
                    deletedSensor.sensorId
                }, minReading: ${tojson(minReading)}`,
            );
        }

        // Revert any migration pausing that was done
        const migrationsNeedReset = retryUntilWorked((options) => {
            return logColl.count({migrationsNeedReset: true}, options) > 0;
        });
        if (migrationsNeedReset) {
            assert.commandWorked(
                db.adminCommand({setClusterParameter: {pauseMigrationsDuringMultiUpdates: {enabled: false}}}),
            );
            cluster.executeOnMongosNodes((db) => {
                // Ensure all mongoses have refreshed cluster parameter after being set.
                assert.soon(() => {
                    const response = assert.commandWorked(
                        db.adminCommand({getClusterParameter: "pauseMigrationsDuringMultiUpdates"}),
                    );
                    return !response.clusterParameters[0].enabled;
                });
            });
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
