/**
 * Tests that $rankFusion/$scoreFusion can't be used on a timeseries collection.
 *
 * @tags: [requires_fcv_82, featureFlagSearchHybridScoringFull, requires_timeseries]
 */

const timeFieldName = "time";
const metaFieldName = "tags";
const timeseriesCollName = jsTestName();
const tsColl = db.getCollection(timeseriesCollName);
tsColl.drop();
assert.commandWorked(
    db.createCollection(timeseriesCollName, {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}),
);

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

const rankFusionPipeline = [
    {
        $rankFusion: {
            input: {
                pipelines: {
                    a: [{$sort: {x: -1}}],
                    b: [{$sort: {x: 1}}],
                },
            },
        },
    },
];

const scoreFusionPipeline = [
    {
        $scoreFusion: {
            input: {
                pipelines: {
                    single: [{$score: {score: "$single", normalization: "minMaxScaler"}}],
                    double: [{$score: {score: "$double", normalization: "none"}}],
                },
                normalization: "none",
            },
            combination: {method: "avg"},
        },
    },
];

// Running $rankFusion on timeseries collection is disallowed.
// TODO SERVER-101599 remove 'ErrorCodes.OptionNotSupportedOnView',
// 'ErrorCodes.CommandNotSupportedOnView', and 10170100 once 9.0 becomes lastLTS, and timeseries
// collections will not have views anymore.
assert.commandFailedWithCode(tsColl.runCommand("aggregate", {pipeline: rankFusionPipeline, cursor: {}}), [
    10557301,
    10557300,
    ErrorCodes.OptionNotSupportedOnView,
    ErrorCodes.CommandNotSupportedOnView,
    10170100,
]);

// Running $rankFusion on timeseries collection is disallowed.
// TODO SERVER-101599 remove 'ErrorCodes.OptionNotSupportedOnView',
// 'ErrorCodes.CommandNotSupportedOnView', and 10170100 once 9.0 becomes lastLTS, and timeseries
// collections will not have views anymore.
assert.commandFailedWithCode(tsColl.runCommand("aggregate", {pipeline: scoreFusionPipeline, cursor: {}}), [
    10557301,
    10557300,
    ErrorCodes.OptionNotSupportedOnView,
    ErrorCodes.CommandNotSupportedOnView,
    10170100,
]);
