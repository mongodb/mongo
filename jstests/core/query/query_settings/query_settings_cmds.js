// Tests query settings setQuerySettings and removeQuerySettings commands as well as $querySettings
// agg stage.
// @tags: [
//   directly_against_shardsvrs_incompatible,
//   featureFlagQuerySettings,
//   does_not_support_stepdowns,
// ]
//

import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

const adminDB = db.getSiblingDB("admin");
const coll = db[jsTestName()];
const nonExistentQueryShapeHash =
    "0000000000000000000000000000000000000000000000000000000000000000";
const querySettingsAggPipeline = [
    {$querySettings: {}},
    {$project: {queryShapeHash: 0}},
    {$sort: {representativeQuery: 1}},
];

/**
 * Makes an query instance of the find command with an optional filter clause.
 */
function makeQueryInstance(filter = {}) {
    return {find: coll.getName(), $db: db.getName(), filter};
}

/**
 * Makes a QueryShapeConfiguration object without the QueryShapeHash.
 */
function makeQueryShapeConfiguration(settings, representativeQuery) {
    return {settings, representativeQuery};
}

const queryA = makeQueryInstance({a: 1});
const queryB = makeQueryInstance({b: "string"});
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

// Ensure that setQuerySettings command fails for invalid input.
{
    assert.commandFailedWithCode(
        db.adminCommand({setQuerySettings: nonExistentQueryShapeHash, settings: querySettingsA}),
        7746401);
    assert.commandFailedWithCode(
        db.adminCommand({setQuerySettings: {notAValid: "query"}, settings: querySettingsA}),
        7746402);
    assert.commandFailedWithCode(
        db.adminCommand({setQuerySettings: makeQueryInstance(), settings: {notAValid: "settings"}}),
        40415);
}

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
        {setQuerySettings: makeQueryInstance({b: "test"}), settings: querySettingsA}));
    assertQueryShapeConfiguration([
        makeQueryShapeConfiguration(querySettingsB, queryA),
        makeQueryShapeConfiguration(querySettingsA, queryB)
    ]);
}

// Ensure that removeQuerySettings command fails for invalid input.
{
    assert.commandFailedWithCode(db.adminCommand({removeQuerySettings: nonExistentQueryShapeHash}),
                                 7746701);
    assert.commandFailedWithCode(db.adminCommand({removeQuerySettings: {notAValid: "query"}}),
                                 7746402);
}

// Ensure that removeQuerySettings command removes one query settings from the 'settingsArray' of
// the 'querySettings' cluster parameter by providing a query instance.
{
    assert.commandWorked(db.adminCommand({removeQuerySettings: makeQueryInstance({b: "shape"})}));
    assertQueryShapeConfiguration([makeQueryShapeConfiguration(querySettingsB, queryA)]);
}

// Ensure that query settings cluster parameter is empty by issuing a removeQuerySettings command
// providing a query shape hash.
{
    const queryShapeHashA = adminDB.aggregate([{$querySettings: {}}]).toArray()[0].queryShapeHash;
    assert.commandWorked(db.adminCommand({removeQuerySettings: queryShapeHashA}));
    assertQueryShapeConfiguration([]);
}

// Ensure that $querySettings agg stage inherits the constraints from the underlying alias stages,
// including $queue.
{
    assert.commandFailedWithCode(
        db.adminCommand(
            {aggregate: 1, pipeline: [{$documents: []}, {$querySettings: {}}], cursor: {}}),
        40602);
}

// Reset the 'clusterServerParameterRefreshIntervalSecs' parameter to its initial value.
if (FixtureHelpers.isMongos(db)) {
    assert.commandWorked(db.adminCommand({
        setParameter: 1,
        clusterServerParameterRefreshIntervalSecs: initParameterRefreshInterval
    }));
}
