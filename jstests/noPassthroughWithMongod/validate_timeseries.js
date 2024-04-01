/*
 * Tests that calling the validate command on a time-series collection is allowed, while ensuring
 * that calling validate on non-time-series views is still prohibited.
 *
 * @tags: [
 * requires_fcv_62
 * ]
 */
import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";

assertDropCollection(db, "validate_timeseries");
assertDropCollection(db, "viewSource");

assert.commandWorked(db.createCollection(
    "validate_timeseries",
    {timeseries: {timeField: "timestamp", metaField: "metadata", granularity: "hours"}}));

const coll = db.validate_timeseries;
const bucketColl = db.system.buckets.validate_timeseries;
const weather_data = [
    {
        "metadata": {"sensorId": 5578, "type": "temperature"},
        "timestamp": ISODate("2021-05-18T00:00:00.000Z"),
        "temp": 12
    },
    {
        "metadata": {"sensorId": 5578, "type": "temperature"},
        "timestamp": ISODate("2021-05-18T04:00:00.000Z"),
        "temp": 11
    },
];

assert.commandWorked(coll.insertMany(weather_data), {ordered: false});

// Tests that the validate command can be run on a time-series collection and its underlying buckets
// collection.

let res = assert.commandWorked(coll.validate());
assert(res.valid, tojson(res));

res = assert.commandWorked(bucketColl.validate());
assert(res.valid, tojson(res));

// Tests that the validate command doesn't run on a view without a bucket collection.
assert.commandWorked(db.createCollection("viewSource"));

assert.commandWorked(db.createView("view", "viewSource", [{$project: {"Name": "$temp"}}]));

const viewSource = db.viewSource;
const view = db.view;

res = assert.commandWorked(viewSource.validate());
assert(res.valid, tojson(res));

assert.commandFailedWithCode(view.validate(), ErrorCodes.CommandNotSupportedOnView);
