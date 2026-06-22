/**
 * Tests that bypassDocumentValidation:true is not allowed when writing directly to timeseries
 * buckets.
 * Tests that bypassDocumentValidation:true is not allowed when writing directly to timeseries
 * buckets.
 *
 * @tags: [
 *   requires_timeseries,
 * ]
 */

const coll = db[jsTestName()];
const bucketsColl = db["system.buckets." + jsTestName()];

coll.drop();

const timeField = "t";
const metaField = "m";

assert.commandWorked(db.createCollection(
    coll.getName(), {timeseries: {timeField: timeField, metaField: metaField}}));

// Insert a measurement to create a bucket.
assert.commandWorked(
    coll.insert({[timeField]: ISODate("2024-01-01T00:00:00Z"), [metaField]: 1, v: 1}));

// Retrieve the raw bucket so we have a valid bucket document to use in write operations.
const bucket = bucketsColl.find().toArray()[0];
assert(bucket, "Expected to find at least one bucket");

// Delete the bucket so we can re-insert it as-is.
assert.commandWorked(bucketsColl.deleteOne({_id: bucket._id}));

// Verify that bypassDocumentValidation: true is rejected for inserts via the raw interface.
assert.commandFailedWithCode(
    db.runCommand({
        insert: bucketsColl.getName(),
        documents: [bucket],
        bypassDocumentValidation: true,
    }),
    ErrorCodes.BadValue,
    "Expected insert with bypassDocumentValidation:true to fail on raw bucket writes",
);

// Insert the bucket back without bypassDocumentValidation so the update test has a document to work
// with.
assert.commandWorked(bucketsColl.insertOne(bucket));

// Verify that bypassDocumentValidation: true is rejected for updates via the raw interface.
assert.commandFailedWithCode(
    db.runCommand({
        update: bucketsColl.getName(),
        updates: [{q: {_id: bucket._id}, u: {$set: {meta: 2}}}],
        bypassDocumentValidation: true,
    }),
    ErrorCodes.BadValue,
    "Expected update with bypassDocumentValidation:true to fail on raw bucket writes",
);

// The same update with explicit bypassDocumentValidation: false should succeed.
assert.commandWorked(
    db.runCommand({
        update: bucketsColl.getName(),
        updates: [{q: {_id: bucket._id}, u: {$set: {meta: 2}}}],
        bypassDocumentValidation: false,
    }),
);
