/**
 * Test that the $rank window function can use a bounded sort with the correct sorting metadata.
 *
 * @tags: [featureFlagRankFusionFull]
 */
const tsCollName = jsTestName() + "_ts_coll";
const tsColl = db.getCollection(tsCollName);
tsColl.drop();

const document = {
    metadata: {
        a: 1,
        b: 2,
    },
    time: new Date(1737331200000),  // Mon Jan 21 2025
};
assert.commandWorked(tsColl.insert(document));

const indexForBoundedSort = {
    "metadata.a": 1,
    "time": -1
};
assert.commandWorked(tsColl.createIndex(indexForBoundedSort));

const boundedSortPipeline = [
    {$setWindowFields: {sortBy: {"time": 1}, output: {rank: {$documentNumber: {}}}}},
];

assert.commandWorked(tsColl.runCommand("aggregate", {
    pipeline: boundedSortPipeline,
    cursor: {},
}));
