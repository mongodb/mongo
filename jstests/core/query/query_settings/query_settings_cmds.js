// Tests query settings setQuerySettings and removeQuerySettings commands as well as $querySettings
// agg stage.
// @tags: [
//   directly_against_shardsvrs_incompatible,
//   featureFlagQuerySettings,
//   does_not_support_stepdowns,
// ]
//

import {QuerySettingsUtils} from "jstests/core/libs/query_settings_utils.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

const adminDB = db.getSiblingDB("admin");
const coll = db[jsTestName()];

const querySettingsAggPipeline = [
    {$querySettings: {}},
    {$project: {queryShapeHash: 0}},
    {$sort: {representativeQuery: 1}},
];

const utils = new QuerySettingsUtils(db, coll)

/**
 * Makes a QueryShapeConfiguration object without the QueryShapeHash.
 */
function makeQueryShapeConfiguration(settings, representativeQuery) {
    return {settings, representativeQuery};
}

const queryA = utils.makeQueryInstance({a: 1});
const queryB = utils.makeQueryInstance({b: "string"});
const querySettingsA = {
    indexHints: {allowedIndexes: ["a_1", {$natural: 1}]}
};
const querySettingsB = {
    indexHints: {allowedIndexes: ["b_1"]}
};

/**
 * Helper function to assert equality of QueryShapeConfigurations. In order to ease the assertion
 * logic, 'queryShapeHash' field is removed from the QueryShapeConfiguration prior to assertion.
 *
 * Since in sharded clusters the query settings may arrive with a delay to the mongos, the assertion
 * is done via 'assert.soon'.
 */
function assertQueryShapeConfiguration(expectedQueryShapeConfigurations) {
    assert.soon(() => {
        const settingsArray = adminDB.aggregate(querySettingsAggPipeline).toArray();
        return bsonWoCompare(settingsArray, expectedQueryShapeConfigurations) == 0;
    });
}

// Set the 'clusterServerParameterRefreshIntervalSecs' value to 1 second for faster fetching of
// 'querySettings' cluster parameter on mongos from the configsvr.
let initParameterRefreshInterval = 0;
if (FixtureHelpers.isMongos(db)) {
    const response = assert.commandWorked(
        db.adminCommand({getParameter: 1, clusterServerParameterRefreshIntervalSecs: 1}));
    initParameterRefreshInterval = response.clusterServerParameterRefreshIntervalSecs;
    assert.commandWorked(
        db.adminCommand({setParameter: 1, clusterServerParameterRefreshIntervalSecs: 1}));
}

// Ensure that query settings cluster parameter is empty.
{ assertQueryShapeConfiguration([]); }

// Ensure that 'querySettings' cluster parameter contains QueryShapeConfiguration after invoking
// setQuerySettings command.
{
    assert.commandWorked(db.adminCommand({setQuerySettings: queryA, settings: querySettingsA}));
    assertQueryShapeConfiguration([makeQueryShapeConfiguration(querySettingsA, queryA)]);
}

// Ensure that 'querySettings' cluster parameter contains both QueryShapeConfigurations after
// invoking setQuerySettings command.
{
    assert.commandWorked(db.adminCommand({setQuerySettings: queryB, settings: querySettingsB}));
    assertQueryShapeConfiguration([
        makeQueryShapeConfiguration(querySettingsA, queryA),
        makeQueryShapeConfiguration(querySettingsB, queryB)
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
    assertQueryShapeConfiguration([
        makeQueryShapeConfiguration(querySettingsB, queryA),
        makeQueryShapeConfiguration(querySettingsB, queryB)
    ]);
}

// Ensure that 'querySettings' cluster parameter gets updated on subsequent call of setQuerySettings
// by passing a different QueryInstance with the same QueryShape.
{
    assert.commandWorked(db.adminCommand(
        {setQuerySettings: utils.makeQueryInstance({b: "test"}), settings: querySettingsA}));
    assertQueryShapeConfiguration([
        makeQueryShapeConfiguration(querySettingsB, queryA),
        makeQueryShapeConfiguration(querySettingsA, queryB)
    ]);
}

// Ensure that removeQuerySettings command removes one query settings from the 'settingsArray' of
// the 'querySettings' cluster parameter by providing a query instance.
{
    assert.commandWorked(
        db.adminCommand({removeQuerySettings: utils.makeQueryInstance({b: "shape"})}));
    assertQueryShapeConfiguration([makeQueryShapeConfiguration(querySettingsB, queryA)]);
}

// Ensure that query settings cluster parameter is empty by issuing a removeQuerySettings command
// providing a query shape hash.
{
    const queryShapeHashA = adminDB.aggregate([{$querySettings: {}}]).toArray()[0].queryShapeHash;
    assert.commandWorked(db.adminCommand({removeQuerySettings: queryShapeHashA}));
    assertQueryShapeConfiguration([]);
}

// Reset the 'clusterServerParameterRefreshIntervalSecs' parameter to its initial value.
if (FixtureHelpers.isMongos(db)) {
    assert.commandWorked(db.adminCommand({
        setParameter: 1,
        clusterServerParameterRefreshIntervalSecs: initParameterRefreshInterval
    }));
}
