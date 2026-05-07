/**
 * Tests that bypassDocumentValidation:true is not allowed when writing directly to timeseries buckets.
 *
 * @tags: [
 *   requires_timeseries,
 * ]
 */

import {getTimeseriesCollForRawOps, kRawOperationSpec} from "jstests/core/libs/raw_operation_utils.js";

const coll = db[jsTestName()];
coll.drop();

const timeField = "t";
const metaField = "m";

assert.commandWorked(db.createCollection(coll.getName(), {timeseries: {timeField: timeField, metaField: metaField}}));

// Insert a measurement to create a bucket.
assert.commandWorked(coll.insert({[timeField]: ISODate("2024-01-01T00:00:00Z"), [metaField]: 1, v: 1}));

// Retrieve the raw bucket so we have a valid bucket document to use in write operations.
const bucket = getTimeseriesCollForRawOps(coll).find().rawData().toArray()[0];
assert(bucket, "Expected to find at least one bucket");

// Delete the bucket so we can re-insert it as-is.
assert.commandWorked(getTimeseriesCollForRawOps(coll).deleteOne({_id: bucket._id}, kRawOperationSpec));

// Verify that bypassDocumentValidation: true is rejected for inserts via the raw interface.
assert.commandFailedWithCode(
    db.runCommand({
        insert: getTimeseriesCollForRawOps(coll).getName(),
        documents: [bucket],
        ...kRawOperationSpec,
        bypassDocumentValidation: true,
    }),
    ErrorCodes.BadValue,
    "Expected insert with bypassDocumentValidation:true to fail on raw bucket writes",
);

// Insert the bucket back without bypassDocumentValidation so the update test has a document to work with.
assert.commandWorked(getTimeseriesCollForRawOps(coll).insertOne(bucket, kRawOperationSpec));

// Verify that bypassDocumentValidation: true is rejected for updates via the raw interface.
assert.commandFailedWithCode(
    db.runCommand({
        update: getTimeseriesCollForRawOps(coll).getName(),
        updates: [{q: {_id: bucket._id}, u: {$set: {meta: 2}}}],
        ...kRawOperationSpec,
        bypassDocumentValidation: true,
    }),
    ErrorCodes.BadValue,
    "Expected update with bypassDocumentValidation:true to fail on raw bucket writes",
);

// The same update with explicit bypassDocumentValidation: false should succeed.
assert.commandWorked(
    db.runCommand({
        update: getTimeseriesCollForRawOps(coll).getName(),
        updates: [{q: {_id: bucket._id}, u: {$set: {meta: 2}}}],
        ...kRawOperationSpec,
        bypassDocumentValidation: false,
    }),
);
