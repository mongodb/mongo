// Tests query settings are applied regardless of the query engine (SBE or classic).
// @tags: [
//   directly_against_shardsvrs_incompatible,
//   featureFlagQuerySettings,
//   does_not_support_stepdowns,
//   simulate_atlas_proxy_incompatible,
//   cqf_incompatible,
// ]
//

import {getPlanStages, getWinningPlan} from "jstests/libs/analyze_plan.js";
import {QuerySettingsUtils} from "jstests/libs/query_settings_utils.js";

const coll = db[jsTestName()];
const qsutils = new QuerySettingsUtils(db, coll.getName());

// Create the collection, because some sharding passthrough suites are failing when explain
// command is issued on the nonexistent database and collection.
assert.commandWorked(db.createCollection(coll.getName()));
assert.commandWorked(coll.createIndexes([{a: 1}, {b: 1}, {a: 1, b: 1}]));

// Insert data into the collection.
assert.commandWorked(coll.insertMany([
    {a: 1, b: 5},
    {a: 2, b: 4},
    {a: 3, b: 3},
    {a: 4, b: 2},
    {a: 5, b: 1},
]))

const query = qsutils.makeFindQueryInstance({a: 1, b: 1});
const querySettingsA = {
    indexHints: {allowedIndexes: ["a_1"]}
};
const querySettingsB = {
    indexHints: {allowedIndexes: ["b_1"]}
};
const querySettingsAB = {
    indexHints: {allowedIndexes: ["a_1_b_1"]}
};

// Set the 'clusterServerParameterRefreshIntervalSecs' value to 1 second for faster fetching of
// 'querySettings' cluster parameter on mongos from the configsvr.
const clusterParamRefreshSecs = qsutils.setClusterParamRefreshSecs(1);

// Ensure that query settings cluster parameter is empty.
qsutils.assertQueryShapeConfiguration([]);

for (let settings of [querySettingsA, querySettingsB, querySettingsAB]) {
    // Set query settings for a query to use 'settings.indexHints.allowedIndexes' index.
    assert.commandWorked(db.adminCommand({setQuerySettings: query, settings: settings}));
    qsutils.assertQueryShapeConfiguration([qsutils.makeQueryShapeConfiguration(settings, query)]);

    // Ensure that 'settings.indexHints.allowedIndexes' index is used when running the query.
    const explain = coll.find({a: 1, b: 5}).explain();
    const ixscanStages = getPlanStages(getWinningPlan(explain.queryPlanner), "IXSCAN");
    assert.gte(ixscanStages.length, 1, explain);
    const expectedIndexName = settings.indexHints.allowedIndexes[0];
    for (let ixscanStage of ixscanStages) {
        assert.docEq(ixscanStage.indexName, expectedIndexName, explain);
    }
}

// Ensure that the hint gets ignored when query settings for the particular query are specified.
{
    const explain = coll.find({a: 1, b: 5}).hint({a: 1}).explain();
    jsTestLog(explain);
    const ixscanStages = getPlanStages(getWinningPlan(explain.queryPlanner), "IXSCAN");
    assert.gte(ixscanStages.length, 1, explain);

    // 'querySettingsAB' are expected settings, as they are the last settings that are set.
    const expectedIndexName = querySettingsAB.indexHints.allowedIndexes[0];
    for (let ixscanStage of ixscanStages) {
        assert.docEq(ixscanStage.indexName, expectedIndexName, explain);
    }
}

qsutils.removeAllQuerySettings();

// Reset the 'clusterServerParameterRefreshIntervalSecs' parameter to its initial value.
clusterParamRefreshSecs.restore();
