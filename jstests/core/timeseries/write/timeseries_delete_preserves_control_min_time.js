/**
 * Verifies that deleting the earliest measurement in a time-series bucket does not change
 * control.min.time (the shard key for collections sharded on time).
 *
 * @tags: [
 *   requires_timeseries,
 *   requires_non_retryable_writes,
 *   does_not_support_stepdowns,
 * ]
 */

import {getTimeseriesCollForRawOps} from "jstests/core/libs/raw_operation_utils.js";
import {afterEach, describe, it} from "jstests/libs/mochalite.js";

const testDB = db.getSiblingDB(jsTestName());

describe("Deleting the earliest measurement preserves control.min.time", () => {
    let coll;

    afterEach(() => {
        coll.drop();
    });

    it("deleteOne of the earliest measurement does not change control.min.time", () => {
        coll = testDB["ts"];
        assert.commandWorked(
            testDB.createCollection(coll.getName(), {
                timeseries: {timeField: "t", metaField: "m", granularity: "seconds"},
            }),
        );

        // With "seconds" granularity, control.min.time is rounded down to the nearest minute.
        // We choose timestamps so that:
        //   - The earliest (t0) rounds to minute :05
        //   - The remaining two (t1, t2) round to minute :30 and :45
        // After deleting t0, the natural new minimum would round to :30 -- different from :05.
        // The fix must preserve control.min.time at :05 regardless.
        const t0 = ISODate("2024-01-01T00:05:10Z"); // earliest; rounds to 00:05
        const t1 = ISODate("2024-01-01T00:30:00Z"); // rounds to 00:30
        const t2 = ISODate("2024-01-01T00:45:00Z"); // rounds to 00:45

        assert.commandWorked(
            coll.insertMany([
                {_id: 0, t: t0, m: "a", v: 1},
                {_id: 1, t: t1, m: "a", v: 2},
                {_id: 2, t: t2, m: "a", v: 3},
            ]),
        );

        const bucketsBefore = getTimeseriesCollForRawOps(coll).find().rawData().toArray();
        assert.eq(1, bucketsBefore.length, "expected one bucket: " + tojson(bucketsBefore));
        const minTimeBefore = bucketsBefore[0].control.min.t;

        // Delete the earliest measurement (the one that determines control.min.time).
        assert.eq(1, coll.deleteOne({_id: 0}).deletedCount);

        const bucketsAfter = getTimeseriesCollForRawOps(coll).find().rawData().toArray();
        assert.eq(1, bucketsAfter.length, "expected one bucket after delete: " + tojson(bucketsAfter));

        assert.eq(
            minTimeBefore,
            bucketsAfter[0].control.min.t,
            "control.min.time changed after deleting the earliest measurement",
        );
    });

    it("deleteMany of the earliest measurements does not change control.min.time", () => {
        coll = testDB["ts_multi"];
        assert.commandWorked(
            testDB.createCollection(coll.getName(), {
                timeseries: {timeField: "t", metaField: "m", granularity: "seconds"},
            }),
        );

        const t0 = ISODate("2024-01-01T00:05:10Z");
        const t1 = ISODate("2024-01-01T00:05:20Z");
        const t2 = ISODate("2024-01-01T00:30:00Z");

        assert.commandWorked(
            coll.insertMany([
                {_id: 0, t: t0, m: "a", v: 1},
                {_id: 1, t: t1, m: "a", v: 2},
                {_id: 2, t: t2, m: "a", v: 3},
            ]),
        );

        const bucketsBefore = getTimeseriesCollForRawOps(coll).find().rawData().toArray();
        const minTimeBefore = bucketsBefore[0].control.min.t;

        // Delete both early measurements.
        assert.eq(2, coll.deleteMany({v: {$lt: 3}}).deletedCount);

        const bucketsAfter = getTimeseriesCollForRawOps(coll).find().rawData().toArray();
        assert.eq(1, bucketsAfter.length);
        assert.eq(
            minTimeBefore,
            bucketsAfter[0].control.min.t,
            "control.min.time changed after deleteMany of earliest measurements",
        );
    });
});
