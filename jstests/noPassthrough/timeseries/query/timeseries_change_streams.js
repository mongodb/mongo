// Tests that $changeStream aggregations against time-series collections fail cleanly.
// @tags: [
//  requires_timeseries,
//  requires_replication,
// ]

import {getRawOperationSpec, getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const timeFieldName = "time";
const metaFieldName = "tags";
const testDB = rst.getPrimary().getDB(jsTestName());
assert.commandWorked(testDB.dropDatabase());

const tsColl = testDB.getCollection("ts_point_data");
tsColl.drop();

assert.commandWorked(
    testDB.createCollection(tsColl.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}),
);

const nMeasurements = 10;

for (let i = 0; i < nMeasurements; i++) {
    const docToInsert = {
        time: ISODate(),
        tags: i.toString(),
        value: i + nMeasurements,
    };
    assert.commandWorked(tsColl.insert(docToInsert));
}

// Test that a changeStream cannot be opened on a time-series collection because it's a view, both
// with and without rawData.
assert.throwsWithCode(
    () => getTimeseriesCollForRawOps(testDB, tsColl).aggregate([{$changeStream: {}}], getRawOperationSpec(testDB)),
    ErrorCodes.CommandNotSupportedOnView,
);
assert.commandFailedWithCode(
    testDB.runCommand({aggregate: tsColl.getName(), pipeline: [{$changeStream: {}}], cursor: {}}),
    ErrorCodes.CommandNotSupportedOnView,
);

rst.stopSet();
