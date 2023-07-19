// Tests query settings setQuerySettings and removeQuerySettings commands as well as $querySettings
// agg stage.
// @tags: [
//   directly_against_shardsvrs_incompatible,
//   featureFlagQuerySettings,
//   does_not_support_stepdowns,
// ]
//
(function() {
'use strict';

load("jstests/libs/fixture_helpers.js");

const adminDB = db.getSiblingDB("admin");
const coll = db[jsTestName()];
const queryA = {
    find: coll.getName(),
    $db: db.getName(),
    filter: {a: 1}
};
const queryB = {
    find: coll.getName(),
    $db: db.getName(),
    filter: {b: "string"}
};
const nonExistentQueryShapeHash =
    "0000000000000000000000000000000000000000000000000000000000000000";
const querySettingsA = {
    indexHints: {allowedIndexes: ["a_1", {$natural: 1}]}
};
const querySettingsB = {
    indexHints: {allowedIndexes: ["b_1"]}
};
const queryShapeConfigurationA = {
    settings: querySettingsA,
    representativeQuery: queryA
};
const queryShapeConfigurationB = {
    settings: querySettingsB,
    representativeQuery: queryB
};
const querySettingsAggPipeline = [
    {$querySettings: {}},
    {$project: {queryShapeHash: 0}},
    {$sort: {representativeQuery: 1}},
];

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
        db.adminCommand({setQuerySettings: queryA, settings: {notAValid: "settings"}}), 40415);
}

// Ensure that 'querySettings' cluster parameter contains QueryShapeConfiguration after invoking
// setQuerySettings command.
{
    assert.commandWorked(db.adminCommand({setQuerySettings: queryA, settings: querySettingsA}));
    assertQueryShapeConfiguration([queryShapeConfigurationA]);
}

// Ensure that 'querySettings' cluster parameter contains both QueryShapeConfigurations after
// invoking setQuerySettings command.
{
    assert.commandWorked(db.adminCommand({setQuerySettings: queryB, settings: querySettingsB}));
    assertQueryShapeConfiguration([queryShapeConfigurationA, queryShapeConfigurationB]);
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
    assert.commandWorked(db.adminCommand({removeQuerySettings: queryB}));
    assertQueryShapeConfiguration([queryShapeConfigurationA]);
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
})();
