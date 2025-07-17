/**
 * Tests that $rankFusion can't be used on a timeseries collection.
 *
 * @tags: [requires_fcv_81, requires_timeseries]
 */

const timeFieldName = "time";
const metaFieldName = "tags";
const timeseriesCollName = jsTestName();
const tsColl = db.getCollection(timeseriesCollName);
tsColl.drop();
assert.commandWorked(db.createCollection(
    timeseriesCollName, {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));

const nDocs = 10;
const bulk = tsColl.initializeUnorderedBulkOp();
for (let i = 0; i < nDocs; i++) {
    const docToInsert = {
        time: ISODate(),
        tags: {loc: [40, 40], descr: i.toString()},
        value: i + nDocs,
    };
    bulk.insert(docToInsert);
}
assert.commandWorked(bulk.execute());

const rankFusionPipeline = [{
    $rankFusion: {
        input: {
            pipelines: {
                a: [{$sort: {x: -1}}],
                b: [{$sort: {x: 1}}],
            }
        }
    }
}];

// Running $rankFusion on timeseries collection is disallowed.
// TODO SERVER-101599 remove 'ErrorCodes.OptionNotSupportedOnView',
// 'ErrorCodes.CommandNotSupportedOnView', and 10170100 once 9.0 becomes lastLTS, and timeseries
// collections will not have views anymore.
assert.commandFailedWithCode(
    tsColl.runCommand("aggregate", {pipeline: rankFusionPipeline, cursor: {}}), [
        10557301,
        10557300,
        ErrorCodes.OptionNotSupportedOnView,
        ErrorCodes.CommandNotSupportedOnView,
        10170100
    ]);
