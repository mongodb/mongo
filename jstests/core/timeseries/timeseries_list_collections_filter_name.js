/**
 * Tests that listCollections includes time-series collections and their options when filtering on
 * name.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 */
const timeFieldName = 'time';

const coll = db[jsTestName()];
coll.drop();

assert.commandWorked(db.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}));

const collections =
    assert.commandWorked(db.runCommand({listCollections: 1, filter: {name: coll.getName()}}))
        .cursor.firstBatch;

const timeseriesOptions = {
    timeField: timeFieldName,
    granularity: 'seconds',
    bucketMaxSpanSeconds: 3600
};

const collectionOptions = [{
    name: coll.getName(),
    type: 'timeseries',
    options: {timeseries: timeseriesOptions},
    info: {readOnly: false},
}];

assert.eq(collections, collectionOptions);