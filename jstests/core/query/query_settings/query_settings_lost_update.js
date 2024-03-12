/**
 * Tests concurrent updates of query settings.
 * Excluding test suites that do not expect parallel shell or connect to shards directly
 * ('setClusterParameter' can only run on mongos in sharded clusters).
 * @tags: [
 *   command_not_supported_in_serverless,
 *   directly_against_shardsvrs_incompatible,
 *   featureFlagQuerySettings,
 *   tenant_migration_incompatible,
 *   uses_parallel_shell,
 * ]
 */

import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {QuerySettingsUtils} from "jstests/libs/query_settings_utils.js";

// Early exit in case we are running on standalone mongod. Standalone mongod does not contain a
// functioning 'VectorClock' instance. The check we have introduced in this change relies on a
// functioning vector clock.
if (!FixtureHelpers.isMongos(db) && !db.runCommand({hello: 1}).hasOwnProperty('setName')) {
    quit();
}

const testConn = db.getMongo();
// (Re)create the collection - will be sharded if required.
const collName = jsTestName();
const coll = assertDropAndRecreateCollection(db, collName);
const ns = {
    db: db.getName(),
    coll: collName
};
const qsutils = new QuerySettingsUtils(db, collName);
const queryA = qsutils.makeFindQueryInstance({filter: {a: 1}});
const queryB = qsutils.makeFindQueryInstance({filter: {b: "string"}});
const queryC = qsutils.makeFindQueryInstance({filter: {c: 1}});
const querySettingsA = {
    indexHints: {ns, allowedIndexes: ["a_1", {$natural: 1}]}
};
const querySettingsB = {
    indexHints: {ns, allowedIndexes: ["b_1"]}
};
const querySettingsC = {
    indexHints: {ns, allowedIndexes: ["c_1"]}
};

// TODO SERVER-85242 Remove once the fallback mechanism is re-implemented.
for (const indexKeyPattern of [{a: 1}, {b: 1}, {c: 1}]) {
    assert.commandWorked(coll.createIndex(indexKeyPattern));
}
for (let i = 0; i < 10; i++) {
    coll.insert({a: i, b: i, c: i});
}

function runSetQuerySettingsConcurrently(
    {initialConfiguration, settingToFail, settingToPass, finalConfiguration}) {
    qsutils.assertQueryShapeConfiguration(initialConfiguration);

    const hangSetParamFailPoint = configureFailPoint(testConn, "hangInSetClusterParameter", {
        "querySettings.settingsArray.representativeQuery": settingToFail.representativeQuery
    });

    // Set 'settingToFail' in a parallel shell. This command will hang because of the active
    // failpoint.
    const waitForSettingToFail = startParallelShell(
        funWithArgs((query, settings) => {
            return assert.commandFailedWithCode(
                db.adminCommand({setQuerySettings: query, settings: settings}),
                ErrorCodes.ConflictingOperationInProgress);
        }, settingToFail.representativeQuery, settingToFail.settings), testConn.port);

    // Wait until the failpoint is hit.
    hangSetParamFailPoint.wait();

    // Set 'settingToPass' in a parrallel shell. This command will succeed, because of the
    // failpoint's configuration.
    const waitForSettingToPass = startParallelShell(
        funWithArgs((query, settings) => {
            return assert.commandWorked(
                db.adminCommand({setQuerySettings: query, settings: settings}));
        }, settingToPass.representativeQuery, settingToPass.settings), testConn.port);

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

// Ensure that query settings cluster parameter is empty at the end of the test.
qsutils.removeAllQuerySettings();
