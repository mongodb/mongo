// Tests concurrent updates of query settings.
// @tags: [
//   featureFlagQuerySettings,
// ]
//

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {QuerySettingsUtils} from "jstests/libs/query_settings_utils.js";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const testDB = rst.getPrimary().getDB("test");
const qsutils = new QuerySettingsUtils(testDB, jsTestName());

const queryA = qsutils.makeQueryInstance({a: 1});
const queryB = qsutils.makeQueryInstance({b: "string"});
const queryC = qsutils.makeQueryInstance({c: 1});
const querySettingsA = {
    indexHints: {allowedIndexes: ["a_1", {$natural: 1}]}
};
const querySettingsB = {
    indexHints: {allowedIndexes: ["b_1"]}
};
const querySettingsC = {
    indexHints: {allowedIndexes: ["c_1"]}
};

// Set the 'clusterServerParameterRefreshIntervalSecs' value to 1 second for faster fetching of
// 'querySettings' cluster parameter on mongos from the configsvr.
const clusterParamRefreshSecs = qsutils.setClusterParamRefreshSecs(1);

function runSetQuerySettingsConcurrently(
    {initialConfiguration, settingToFail, settingToPass, finalConfiguration}) {
    qsutils.assertQueryShapeConfiguration(initialConfiguration);

    const hangSetParamFailPoint = configureFailPoint(testDB, "hangInSetClusterParameter", {
        "settingsArray.representativeQuery": settingToFail.representativeQuery
    });

    // Set 'settingToFail' in a parallel shell. This command will hang because of the active
    // failpoint.
    const waitForSettingToFail = startParallelShell(
        funWithArgs((query, settings) => {
            return assert.commandFailedWithCode(
                db.adminCommand({setQuerySettings: query, settings: settings}),
                ErrorCodes.ConflictingOperationInProgress);
        }, settingToFail.representativeQuery, settingToFail.settings), rst.getPrimary().port);

    // Wait until the failpoint is hit.
    hangSetParamFailPoint.wait();

    // Set 'settingToPass' in a parrallel shell. This command will succeed, because of the
    // failpoint's configuration.
    const waitForSettingToPass = startParallelShell(
        funWithArgs((query, settings) => {
            return assert.commandWorked(
                db.adminCommand({setQuerySettings: query, settings: settings}));
        }, settingToPass.representativeQuery, settingToPass.settings), rst.getPrimary().port);

    waitForSettingToPass();

    // Unblock the 'settingToFail' thread.
    hangSetParamFailPoint.off();

    waitForSettingToFail();

    qsutils.assertQueryShapeConfiguration(finalConfiguration);
}

{
    // Ensure a concurrent insert fails when setting 'querySettings' cluster parameter value for the
    // first time.
    runSetQuerySettingsConcurrently({
        initialConfiguration: [],
        settingToFail: qsutils.makeQueryShapeConfiguration(querySettingsA, queryA),
        settingToPass: qsutils.makeQueryShapeConfiguration(querySettingsB, queryB),
        finalConfiguration: [
            qsutils.makeQueryShapeConfiguration(querySettingsB, queryB),
        ]
    });
}

{
    // Ensure a concurrent update fails when updating existing 'querySetings' cluster parameter
    // value.
    runSetQuerySettingsConcurrently({
        initialConfiguration: [
            qsutils.makeQueryShapeConfiguration(querySettingsB, queryB),
        ],
        settingToFail: qsutils.makeQueryShapeConfiguration(querySettingsA, queryA),
        settingToPass: qsutils.makeQueryShapeConfiguration(querySettingsC, queryC),
        finalConfiguration: [
            qsutils.makeQueryShapeConfiguration(querySettingsC, queryC),
            qsutils.makeQueryShapeConfiguration(querySettingsB, queryB),
        ]
    });
}

{
    // Ensure that query settings cluster parameter is empty at the end of the test.
    qsutils.removeAllQuerySettings();
    qsutils.assertQueryShapeConfiguration([]);
}

// Reset the 'clusterServerParameterRefreshIntervalSecs' parameter to its initial value.
clusterParamRefreshSecs.restore();
rst.stopSet();
