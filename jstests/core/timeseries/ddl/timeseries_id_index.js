/**
 * Verifies that the _id index can be created on a timeseries collection.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 */
import {createRawTimeseriesIndex} from "jstests/core/libs/raw_operation_utils.js";
const coll = db[jsTestName()];
coll.drop();

const timeFieldName = "time";
assert.commandWorked(db.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}));

assert.commandWorked(createRawTimeseriesIndex(coll, {"_id": 1}));
assert.commandWorked(createRawTimeseriesIndex(coll, {"_id": 1}, {clustered: true, unique: true}));

// Passing 'clustered' without unique, regardless of the type of clustered collection, is illegal.
assert.commandFailedWithCode(createRawTimeseriesIndex(coll, {"_id": 1}, {clustered: true}),
                             ErrorCodes.CannotCreateIndex);