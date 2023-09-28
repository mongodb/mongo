// Tests query settings setQuerySettings and removeQuerySettings commands as well as $querySettings
// agg stage.
// @tags: [
//   directly_against_shardsvrs_incompatible,
//   featureFlagQuerySettings,
//   does_not_support_stepdowns,
//   simulate_atlas_proxy_incompatible
// ]
//

import {QuerySettingsUtils} from "jstests/libs/query_settings_utils.js";

const qsutils = new QuerySettingsUtils(db, jsTestName());

// Creating the collection, because some sharding passthrough suites are failing when explain
// command is issued on the nonexistent database and collection.
assert.commandWorked(db.createCollection(jsTestName()));

const adminDB = db.getSiblingDB("admin");

const queryA = qsutils.makeQueryInstance({a: 1});
const queryB = qsutils.makeQueryInstance({b: "string"});
const querySettingsA = {
    indexHints: {allowedIndexes: ["a_1", {$natural: 1}]}
};
const querySettingsB = {
    indexHints: {allowedIndexes: ["b_1"]}
};

// Set the 'clusterServerParameterRefreshIntervalSecs' value to 1 second for faster fetching of
// 'querySettings' cluster parameter on mongos from the configsvr.
const clusterParamRefreshSecs = qsutils.setClusterParamRefreshSecs(1);

// Ensure that query settings cluster parameter is empty.
{ qsutils.assertQueryShapeConfiguration([]); }

// Ensure that 'querySettings' cluster parameter contains QueryShapeConfiguration after invoking
// setQuerySettings command.
{
    qsutils.assertExplainQuerySettings(queryA, undefined);
    assert.commandWorked(db.adminCommand({setQuerySettings: queryA, settings: querySettingsA}));
    qsutils.assertQueryShapeConfiguration(
        [qsutils.makeQueryShapeConfiguration(querySettingsA, queryA)]);
}

// Ensure that 'querySettings' cluster parameter contains both QueryShapeConfigurations after
// invoking setQuerySettings command.
{
    qsutils.assertExplainQuerySettings(queryB, undefined);
    assert.commandWorked(db.adminCommand({setQuerySettings: queryB, settings: querySettingsB}));
    qsutils.assertQueryShapeConfiguration([
        qsutils.makeQueryShapeConfiguration(querySettingsA, queryA),
        qsutils.makeQueryShapeConfiguration(querySettingsB, queryB)
    ]);
}

// Ensure that 'querySettings' cluster parameter gets updated on subsequent call of setQuerySettings
// by passing a QueryShapeHash.
{
    const queryShapeHashA =
        adminDB.aggregate([{$querySettings: {}}, {$sort: {representativeQuery: 1}}])
            .toArray()[0]
            .queryShapeHash;
    assert.commandWorked(
        db.adminCommand({setQuerySettings: queryShapeHashA, settings: querySettingsB}));
    qsutils.assertQueryShapeConfiguration([
        qsutils.makeQueryShapeConfiguration(querySettingsB, queryA),
        qsutils.makeQueryShapeConfiguration(querySettingsB, queryB)
    ]);
}

// Ensure that 'querySettings' cluster parameter gets updated on subsequent call of setQuerySettings
// by passing a different QueryInstance with the same QueryShape.
{
    assert.commandWorked(db.adminCommand(
        {setQuerySettings: qsutils.makeQueryInstance({b: "test"}), settings: querySettingsA}));
    qsutils.assertQueryShapeConfiguration([
        qsutils.makeQueryShapeConfiguration(querySettingsB, queryA),
        qsutils.makeQueryShapeConfiguration(querySettingsA, queryB)
    ]);
}

// Ensure that removeQuerySettings command removes one query settings from the 'settingsArray' of
// the 'querySettings' cluster parameter by providing a query instance.
{
    assert.commandWorked(
        db.adminCommand({removeQuerySettings: qsutils.makeQueryInstance({b: "shape"})}));
    qsutils.assertQueryShapeConfiguration(
        [qsutils.makeQueryShapeConfiguration(querySettingsB, queryA)]);
    qsutils.assertExplainQuerySettings(queryB, undefined);
}

// Ensure that query settings cluster parameter is empty by issuing a removeQuerySettings command
// providing a query shape hash.
{
    qsutils.removeAllQuerySettings();
    qsutils.assertExplainQuerySettings(queryA, undefined);
}

// Reset the 'clusterServerParameterRefreshIntervalSecs' parameter to its initial value.
clusterParamRefreshSecs.restore();
