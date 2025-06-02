/**
 * Checks that violations of key constraints cause an index build to fail on time-series
 * collections and the build terminates immediately.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 */
import {createRawTimeseriesIndex} from "jstests/core/libs/raw_operation_utils.js";
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";

TimeseriesTest.run((insert) => {
    const coll = db[jsTestName()];
    coll.drop();

    const timeFieldName = "time";
    assert.commandWorked(
        db.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}));

    for (let i = 0; i < 10; i++) {
        assert.commandWorked(insert(coll, {
            _id: i,
            measurement: "measurement",
            time: ISODate(),
        }));
    }

    assert.commandFailedWithCode(createRawTimeseriesIndex(coll, {"control.min.time": "2dsphere"}),
                                 16755);
});
