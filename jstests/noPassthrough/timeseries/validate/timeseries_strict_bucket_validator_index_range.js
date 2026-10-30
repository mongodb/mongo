/**
 * Tests that the strict time-series bucket validator, which runs on every write to a bucket,
 * rejects an uncompressed bucket whose data field has an index equal to the bucket's measurement
 * count. With N measurements the legal indexes are 0 through N-1.
 *
 * @tags: [requires_fcv_62]
 */

import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {getRawOperationSpec, getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";

const weatherData = [
    {
        metadata: {sensorId: 5578, type: "temperature"},
        timestamp: ISODate("2021-05-18T12:00:00.000Z"),
        temp: 12,
    },
    {
        metadata: {sensorId: 5578, type: "temperature"},
        timestamp: ISODate("2021-05-18T16:00:00.000Z"),
        temp: 16,
    },
    {
        metadata: {sensorId: 5578, type: "temperature"},
        timestamp: ISODate("2021-05-18T20:00:00.000Z"),
        humidity: 15,
    },
];

describe("Strict bucket validator checks data field indexes against the measurement count", function () {
    before(function () {
        this.conn = MongoRunner.runMongod();
        this.db = this.conn.getDB(jsTestName());
        this.testCount = 0;
    });

    beforeEach(function () {
        this.coll = this.db.getCollection("coll" + ++this.testCount);
        assert.commandWorked(
            this.db.createCollection(this.coll.getName(), {
                timeseries: {timeField: "timestamp", metaField: "metadata", granularity: "hours"},
            }),
        );
        assert.commandWorked(this.coll.insertMany(weatherData));

        // Replace the compressed bucket with its uncompressed form so the test can edit the data
        // field indexes directly. The strict validator is left enabled.
        const bucketDoc = getTimeseriesCollForRawOps(this.db, this.coll).findOneWithRawData();
        TimeseriesTest.decompressBucket(bucketDoc);
        assert.commandWorked(
            getTimeseriesCollForRawOps(this.db, this.coll).replaceOne(
                {_id: bucketDoc._id},
                bucketDoc,
                getRawOperationSpec(this.db),
            ),
        );
        assert.eq(3, Object.keys(bucketDoc.data.timestamp).length, {bucketDoc});
    });

    it("accepts the largest legal index, count - 1", function () {
        assert.commandWorked(
            getTimeseriesCollForRawOps(this.db, this.coll).update(
                {},
                {$rename: {"data.temp.1": "data.temp.2"}},
                getRawOperationSpec(this.db),
            ),
        );
    });

    it("rejects an index equal to the measurement count", function () {
        assert.commandFailedWithCode(
            getTimeseriesCollForRawOps(this.db, this.coll).update(
                {},
                {$rename: {"data.temp.1": "data.temp.3"}},
                getRawOperationSpec(this.db),
            ),
            ErrorCodes.DocumentValidationFailure,
        );
    });

    it("rejects an index greater than the measurement count", function () {
        assert.commandFailedWithCode(
            getTimeseriesCollForRawOps(this.db, this.coll).update(
                {},
                {$rename: {"data.temp.1": "data.temp.10"}},
                getRawOperationSpec(this.db),
            ),
            ErrorCodes.DocumentValidationFailure,
        );
    });

    after(function () {
        MongoRunner.stopMongod(this.conn);
    });
});
