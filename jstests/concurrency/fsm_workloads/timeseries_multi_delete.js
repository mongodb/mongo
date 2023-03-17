'use strict';

/**
 * TODO SERVER-73094 remove this test
 *
 * timeseries_multi_delete.js
 *
 * Inserts a bunch of seed data into a time-series collection and then issues a bunch of concurrent
 * multi-deletes. These are designed to overlap in their targets, and the assertion is that
 * conflicting multi-deletes should result in the document being deleted by one thread or the other.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   featureFlagTimeseriesUpdatesDeletesSupport,
 *   requires_non_retryable_writes,
 *   # TODO SERVER-74955: Enable this test.
 *   __TEMPORARILY_DISABLED__,
 * ]
 */

var $config = (function() {
    const data = {nReadingsPerSensor: 300, nSensors: 6};
    const states = {
        deleteMany: function deleteMany(db, collName) {
            db[collName].deleteMany({readingNo: Random.randInt(this.nReadingsPerSensor)});
        },
    };

    var transitions = {deleteMany: {deleteMany: 1}};

    function setup(db, collName, cluster) {
        db[collName].drop();
        db.createCollection(collName, {timeseries: {timeField: "ts", metaField: "sensorId"}});

        // Create a bunch of measurements for different sensors. We will try to create the data in
        // such a way that a multi-delete will try to target one or more measurement from each
        // bucket - this should induce some conflicts on writing to the bucket. 'readingNo' will be
        // our target.
        let bulk = db[collName].initializeUnorderedBulkOp();
        let idCounter = 0;
        for (let sensorId = 0; sensorId < data.nSensors; ++sensorId) {
            for (var i = 0; i < data.nReadingsPerSensor; ++i) {
                bulk.insert(
                    {_id: idCounter++, sensorId: sensorId, readingNo: i, ts: new ISODate()});
            }
        }
        bulk.execute();
    }

    function teardown(db, collName, cluster) {
        for (let readingNo = 0; readingNo < data.nReadingsPerSensor; ++readingNo) {
            const nReadings = db[collName].count({readingNo: readingNo});
            // The way the stage works should detect conflicts with other threads and retry,
            // ensuring that each matching document is indeed removed - at least in this scenario
            // with no indexes, concurrent inserts, or updates.
            assertWhenOwnColl(nReadings == 0 || nReadings == data.nSensors,
                              `Expected all or none of the readings to be deleted: readingNo: ${
                                  readingNo}, nReadings: ${nReadings}, allDocs: ${
                                  tojson(db[collName].find({readingNo: readingNo}).toArray())}`);
        }
    }

    return {
        threadCount: 3,
        iterations: 40,
        data: data,
        states: states,
        startState: "deleteMany",
        transitions: transitions,
        setup: setup,
        teardown: teardown,
    };
})();
