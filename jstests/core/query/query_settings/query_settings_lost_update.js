/**
 * Tests concurrent updates of query settings.
 * Excluding test suites that do not expect parallel shell or connect to shards directly
 * ('setClusterParameter' can only run on mongos in sharded clusters).
 * @tags: [
 *   command_not_supported_in_serverless,
 *   directly_against_shardsvrs_incompatible,
 *   tenant_migration_incompatible,
 *   uses_parallel_shell,
 *   requires_fcv_80,
 * ]
 */

import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {QuerySettingsUtils} from "jstests/libs/query/query_settings_utils.js";

// Early exit in case we are running on standalone mongod. Standalone mongod does not contain a
// functioning 'VectorClock' instance. The check we have introduced in this change relies on a
// functioning vector clock.
if (!FixtureHelpers.isMongos(db) && !db.runCommand({hello: 1}).hasOwnProperty('setName')) {
    quit();
}

const testConn = db.getMongo();
// (Re)create the collection - will be sharded if required.
const collName = jsTestName();
assertDropAndRecreateCollection(db, collName);
const ns = {
    db: db.getName(),
    coll: collName
};
const qsutils = new QuerySettingsUtils(db, collName);
const queryA = qsutils.makeFindQueryInstance({filter: {a: 1}});
const queryAInstance2 = qsutils.makeFindQueryInstance({filter: {a: 2}});
const queryB = qsutils.makeFindQueryInstance({filter: {b: "string"}});
const querySettingsA = {
    indexHints: {ns, allowedIndexes: ["a_1", {$natural: 1}]}
};
const querySettingsB = {
    indexHints: {ns, allowedIndexes: ["b_1"]}
};
const querySettingsC = {
    indexHints: {ns, allowedIndexes: ["c_1"]}
};

/**
 * Tests interleaved execution of two query settings modification commands - 'commandToFail' and
 * 'commandToPass'. 'commandToFail' gets blocked after the query settings cluster-wide configuration
 * option is read for modification.
 *
 * @param initialConfiguration query settings to set before performing the test. An array
 *     of {settings, representativeQuery}.
 * @param commandToFail either "setQuerySettings" or "removeQuerySettings" command which is expected
 *     to fail with 'ConflictingOperationInProgress' error.
 * @param commandToPass either "setQuerySettings" or "removeQuerySettings" command which is runs.
 * @param finalConfiguration expected query settings after the test.
 */
function runQuerySettingsCommandsConcurrently(
    {initialConfiguration = [], commandToFail, commandToPass, finalConfiguration}) {
    qsutils.removeAllQuerySettings();

    // Set the query settings state to one defined by 'initialConfiguration'.
    for (const {settings, representativeQuery} of initialConfiguration) {
        assert.commandWorked(
            db.adminCommand({setQuerySettings: representativeQuery, settings: settings}));
    }

    // Extracts the representative query from either "setQuerySettings" or "removeQuerySettings"
    // command.
    function getRepresentativeQuery(command) {
        if (command.hasOwnProperty("setQuerySettings")) {
            return command.setQuerySettings;
        }
        if (command.hasOwnProperty("removeQuerySettings")) {
            return command.removeQuerySettings;
        }
        assert("Unsupported command " + tojson(command));
    }

    // Program a fail-point to block the query settings modification command 'commandToFail'
    // processing after it reads query settings cluster-wide parameter for update.
    const hangPauseAfterReadingQuerySettingsConfigurationParameterFailPoint =
        configureFailPoint(testConn,
                           "pauseAfterReadingQuerySettingsConfigurationParameter",
                           {representativeQueryToBlock: getRepresentativeQuery(commandToFail)});

    // Run 'commandToFail' command in a parallel shell. This command will hang because of the active
    // fail-point.
    const waitForCommandToFail = startParallelShell(
        funWithArgs((command) => {
            return assert.commandFailedWithCode(db.adminCommand(command),
                                                ErrorCodes.ConflictingOperationInProgress);
        }, commandToFail), testConn.port);

    // Wait until the fail-point is hit.
    hangPauseAfterReadingQuerySettingsConfigurationParameterFailPoint.wait();

    // Run 'commandToPass' command in a parallel shell. This command will succeed, because of the
    // fail-point's configuration.
    const waitForCommandToPass =
        startParallelShell(funWithArgs((command) => {
                               return assert.commandWorked(db.adminCommand(command));
                           }, commandToPass), testConn.port);
    waitForCommandToPass();

    // Unblock 'commandToFail' command.
    hangPauseAfterReadingQuerySettingsConfigurationParameterFailPoint.off();
    waitForCommandToFail();

    // Verify that query settings state matches 'finalConfiguration'.
    qsutils.assertQueryShapeConfiguration(finalConfiguration);
}

// The following test steps verify that optimistic-lock-based concurrency control is correct when
// the query settings cluster-wide configuration option is updated.

// Verify that query settings insert fails when another query settings entry for the same query
// shape has been inserted concurrently.
runQuerySettingsCommandsConcurrently({
    initialConfiguration: [],
    commandToFail: qsutils.makeSetQuerySettingsCommand(
        qsutils.makeQueryShapeConfiguration(querySettingsA, queryA)),
    commandToPass: qsutils.makeSetQuerySettingsCommand(
        qsutils.makeQueryShapeConfiguration(querySettingsC, queryAInstance2)),
    finalConfiguration: [
        qsutils.makeQueryShapeConfiguration(querySettingsC, queryAInstance2),
    ]
});

// Verify that query settings update fails when the query settings entry for the same query shape
// has been updated concurrently.
runQuerySettingsCommandsConcurrently({
    initialConfiguration: [qsutils.makeQueryShapeConfiguration(querySettingsB, queryA)],
    commandToFail: qsutils.makeSetQuerySettingsCommand(
        qsutils.makeQueryShapeConfiguration(querySettingsA, queryA)),
    commandToPass: qsutils.makeSetQuerySettingsCommand(
        qsutils.makeQueryShapeConfiguration(querySettingsC, queryAInstance2)),
    finalConfiguration: [
        qsutils.makeQueryShapeConfiguration(querySettingsC, queryAInstance2),
    ]
});

// Verify that query settings update fails when the query settings entry for the same query shape
// has been deleted concurrently.
runQuerySettingsCommandsConcurrently({
    initialConfiguration: [qsutils.makeQueryShapeConfiguration(querySettingsB, queryA)],
    commandToFail: qsutils.makeSetQuerySettingsCommand(
        qsutils.makeQueryShapeConfiguration(querySettingsA, queryA)),
    commandToPass: qsutils.makeRemoveQuerySettingsCommand(queryAInstance2),
    finalConfiguration: []
});

// Verify that query settings insert fails when another query settings entry for a different query
// shape has been inserted concurrently.
runQuerySettingsCommandsConcurrently({
    initialConfiguration: [],
    commandToFail: qsutils.makeSetQuerySettingsCommand(
        qsutils.makeQueryShapeConfiguration(querySettingsA, queryB)),
    commandToPass: qsutils.makeSetQuerySettingsCommand(
        qsutils.makeQueryShapeConfiguration(querySettingsC, queryAInstance2)),
    finalConfiguration: [
        qsutils.makeQueryShapeConfiguration(querySettingsC, queryAInstance2),
    ]
});

// Verify that query settings update fails when the query settings entry for a different query shape
// has been updated concurrently.
runQuerySettingsCommandsConcurrently({
    initialConfiguration: [
        qsutils.makeQueryShapeConfiguration(querySettingsB, queryA),
        qsutils.makeQueryShapeConfiguration(querySettingsB, queryB)
    ],
    commandToFail: qsutils.makeSetQuerySettingsCommand(
        qsutils.makeQueryShapeConfiguration(querySettingsA, queryB)),
    commandToPass: qsutils.makeSetQuerySettingsCommand(
        qsutils.makeQueryShapeConfiguration(querySettingsC, queryAInstance2)),
    finalConfiguration: [
        qsutils.makeQueryShapeConfiguration(querySettingsC, queryAInstance2),
        qsutils.makeQueryShapeConfiguration(querySettingsB, queryB),
    ]
});

// Verify that query settings update fails when the query settings entry for a different query shape
// has been deleted concurrently.
runQuerySettingsCommandsConcurrently({
    initialConfiguration: [
        qsutils.makeQueryShapeConfiguration(querySettingsB, queryA),
        qsutils.makeQueryShapeConfiguration(querySettingsB, queryB)
    ],
    commandToFail: qsutils.makeSetQuerySettingsCommand(
        qsutils.makeQueryShapeConfiguration(querySettingsA, queryB)),
    commandToPass: qsutils.makeRemoveQuerySettingsCommand(queryAInstance2),
    finalConfiguration: [qsutils.makeQueryShapeConfiguration(querySettingsB, queryB)]
});

// Verify that query settings removal fails when the query settings entry for a different query
// shape has been deleted concurrently.
runQuerySettingsCommandsConcurrently({
    initialConfiguration: [
        qsutils.makeQueryShapeConfiguration(querySettingsB, queryA),
        qsutils.makeQueryShapeConfiguration(querySettingsB, queryB)
    ],
    commandToFail: qsutils.makeRemoveQuerySettingsCommand(queryB),
    commandToPass: qsutils.makeRemoveQuerySettingsCommand(queryAInstance2),
    finalConfiguration: [qsutils.makeQueryShapeConfiguration(querySettingsB, queryB)]
});

qsutils.removeAllQuerySettings();
