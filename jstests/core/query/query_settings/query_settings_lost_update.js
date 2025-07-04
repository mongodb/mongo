/**
 * Tests concurrent updates of query settings.
 * Excluding test suites that do not expect parallel shell or connect to shards directly
 * ('setClusterParameter' can only run on mongos in sharded clusters).
 * @tags: [
 *   # TODO SERVER-98659 Investigate why this test is failing on
 *   # 'sharding_kill_stepdown_terminate_jscore_passthrough'.
 *   does_not_support_stepdowns,
 *   command_not_supported_in_serverless,
 *   directly_against_shardsvrs_incompatible,
 *   uses_parallel_shell,
 *   requires_fcv_80,
 *   # This test assumes 'querySettings' cluster-wide parameter is not modified outside of the test.
 *   # This is not true when running in FCV upgrade/downgrade suite, which involves 'querySettings'
 *   # migration.
 *   # TODO: SERVER-94927 Remove Feature Flag for SPM-3684.
 *   cannot_run_during_upgrade_downgrade,
 * ]
 */

import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {afterEach, before, describe, it} from "jstests/libs/mochalite.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {QuerySettingsUtils} from "jstests/libs/query/query_settings_utils.js";

// Early exit in case we are running on standalone mongod. Standalone mongod does not contain a
// functioning 'VectorClock' instance. The check we have introduced in this change relies on a
// functioning vector clock.
if (!FixtureHelpers.isMongos(db) && !db.runCommand({hello: 1}).hasOwnProperty('setName')) {
    quit();
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

// The following test steps verify that optimistic-lock-based concurrency control is correct when
// the query settings cluster-wide configuration option is updated.
describe("QuerySettings", function() {
    const collName = jsTestName();
    const ns = {db: db.getName(), coll: collName};
    const qsutils = new QuerySettingsUtils(db, collName);
    const queryA = qsutils.makeFindQueryInstance({filter: {a: 1}});
    const queryAInstance2 = qsutils.makeFindQueryInstance({filter: {a: 2}});
    const queryB = qsutils.makeFindQueryInstance({filter: {b: "string"}});
    const querySettingsA = {indexHints: {ns, allowedIndexes: ["a_1", {$natural: 1}]}};
    const querySettingsB = {indexHints: {ns, allowedIndexes: ["b_1"]}};
    const querySettingsC = {indexHints: {ns, allowedIndexes: ["c_1"]}};

    /**
     * Tests interleaved execution of two query settings modification commands -
     * 'commandToHangOnFailpoint' and 'commandToPass'. 'commandToHangOnFailpoint' gets blocked after
     * the query settings cluster-wide configuration option is read for modification.
     *
     * @param initialConfiguration query settings to set before performing the test. An array
     *     of {settings, representativeQuery}.
     * @param commandToHangOnFailpoint either "setQuerySettings" or "removeQuerySettings" command
     *     which will hang its execution on 'failpointToHangOn' failpoint.
     * @param expectedHangedCommandStatusCode expected status code of the
     *     'commandToHangOnFailpoint'.
     * @param commandToPass either "setQuerySettings" or "removeQuerySettings" command which is
     *     runs.
     * @param finalConfiguration expected query settings after the test.
     * @param failpointConfiguration configuration of the fail-point to use during the test.
     */
    function runQuerySettingsCommandsConcurrently({
        initialConfiguration = [],
        commandToHangOnFailpoint,
        expectedHangedCommandStatusCode = ErrorCodes.ConflictingOperationInProgress,
        commandToPass,
        finalConfiguration,
        failpointConfiguration = {
            name: "pauseAfterReadingQuerySettingsConfigurationParameter",
            mode: "alwaysOn",
            data: {representativeQueryToBlock: getRepresentativeQuery(commandToHangOnFailpoint)}
        },
    }) {
        // Set the query settings state to one defined by 'initialConfiguration'.
        for (const {settings, representativeQuery} of initialConfiguration) {
            assert.commandWorked(
                db.adminCommand({setQuerySettings: representativeQuery, settings: settings}));
        }

        // Configure a fail-point to block the query settings modification command
        // 'commandToHangOnFailpoint' processing after it reads query settings cluster-wide
        // parameter for update.
        const waitForHangedCommand = qsutils.withFailpoint(
            failpointConfiguration.name, failpointConfiguration.data, (failpoint, port) => {
                // Run 'commandToHangOnFailpoint' command in a parallel shell. This command will
                // hang because of the active fail-point.
                const waitForHangedCommand = startParallelShell(
                    funWithArgs((command, code) => {
                        if (code > 0) {
                            return assert.commandFailedWithCode(db.adminCommand(command), code);
                        }

                        return assert.commandWorked(db.adminCommand(command));
                    }, commandToHangOnFailpoint, expectedHangedCommandStatusCode), port);

                // Wait until the fail-point is hit.
                failpoint.wait();

                // Run 'commandToPass' command in a parallel shell. This command will succeed,
                // because of the fail-point's configuration.
                const waitForNonHangedCommand =
                    startParallelShell(funWithArgs((command) => {
                                           return assert.commandWorked(db.adminCommand(command));
                                       }, commandToPass), port);
                waitForNonHangedCommand();

                return waitForHangedCommand;
            }, failpointConfiguration.mode);

        // Unblock 'commandToHangOnFailpoint' command.
        waitForHangedCommand();

        // Verify that query settings state matches 'finalConfiguration'.
        qsutils.assertQueryShapeConfiguration(finalConfiguration);
    }

    before(function() {
        assertDropAndRecreateCollection(db, collName);

        // Remove all query settings before running the tests.
        qsutils.removeAllQuerySettings();
    });

    afterEach(function() {
        qsutils.removeAllQuerySettings();
    });

    describe("query settings insert fails", function() {
        it("when another query settings entry for the same query shape has been inserted concurrently.",
           function() {
               runQuerySettingsCommandsConcurrently({
                   initialConfiguration: [],
                   commandToHangOnFailpoint: qsutils.makeSetQuerySettingsCommand(
                       qsutils.makeQueryShapeConfiguration(querySettingsA, queryA)),
                   commandToPass: qsutils.makeSetQuerySettingsCommand(
                       qsutils.makeQueryShapeConfiguration(querySettingsC, queryAInstance2)),
                   finalConfiguration: [
                       qsutils.makeQueryShapeConfiguration(querySettingsC, queryAInstance2),
                   ]
               });
           });

        it("when another query settings entry for a different query shape has been inserted concurrently.",
           function() {
               runQuerySettingsCommandsConcurrently({
                   initialConfiguration: [],
                   commandToHangOnFailpoint: qsutils.makeSetQuerySettingsCommand(
                       qsutils.makeQueryShapeConfiguration(querySettingsA, queryB)),
                   commandToPass: qsutils.makeSetQuerySettingsCommand(
                       qsutils.makeQueryShapeConfiguration(querySettingsC, queryAInstance2)),
                   finalConfiguration: [
                       qsutils.makeQueryShapeConfiguration(querySettingsC, queryAInstance2),
                   ]
               });
           });
    });

    describe("query settings update fails", function() {
        it("when the query settings entry for the same query shape has been updated concurrently",
           function() {
               runQuerySettingsCommandsConcurrently({
                   initialConfiguration:
                       [qsutils.makeQueryShapeConfiguration(querySettingsB, queryA)],
                   commandToHangOnFailpoint: qsutils.makeSetQuerySettingsCommand(
                       qsutils.makeQueryShapeConfiguration(querySettingsA, queryA)),
                   commandToPass: qsutils.makeSetQuerySettingsCommand(
                       qsutils.makeQueryShapeConfiguration(querySettingsC, queryAInstance2)),
                   finalConfiguration: [
                       qsutils.makeQueryShapeConfiguration(querySettingsC, queryAInstance2),
                   ]
               });
           });

        it("when the query settings entry for the same query shape has been deleted concurrently.",
           function() {
               runQuerySettingsCommandsConcurrently({
                   initialConfiguration:
                       [qsutils.makeQueryShapeConfiguration(querySettingsB, queryA)],
                   commandToHangOnFailpoint: qsutils.makeSetQuerySettingsCommand(
                       qsutils.makeQueryShapeConfiguration(querySettingsA, queryA)),
                   commandToPass: qsutils.makeRemoveQuerySettingsCommand(queryAInstance2),
                   finalConfiguration: []
               });
           });

        it("when the query settings entry for a different query shape has been updated concurrently.",
           function() {
               runQuerySettingsCommandsConcurrently({
                   initialConfiguration: [
                       qsutils.makeQueryShapeConfiguration(querySettingsB, queryA),
                       qsutils.makeQueryShapeConfiguration(querySettingsB, queryB)
                   ],
                   commandToHangOnFailpoint: qsutils.makeSetQuerySettingsCommand(
                       qsutils.makeQueryShapeConfiguration(querySettingsA, queryB)),
                   commandToPass: qsutils.makeSetQuerySettingsCommand(
                       qsutils.makeQueryShapeConfiguration(querySettingsC, queryAInstance2)),
                   finalConfiguration: [
                       qsutils.makeQueryShapeConfiguration(querySettingsC, queryAInstance2),
                       qsutils.makeQueryShapeConfiguration(querySettingsB, queryB),
                   ]
               });
           });

        it("when the query settings entry for a different query shape has been deleted concurrently.",
           function() {
               runQuerySettingsCommandsConcurrently({
                   initialConfiguration: [
                       qsutils.makeQueryShapeConfiguration(querySettingsB, queryA),
                       qsutils.makeQueryShapeConfiguration(querySettingsB, queryB)
                   ],
                   commandToHangOnFailpoint: qsutils.makeSetQuerySettingsCommand(
                       qsutils.makeQueryShapeConfiguration(querySettingsA, queryB)),
                   commandToPass: qsutils.makeRemoveQuerySettingsCommand(queryAInstance2),
                   finalConfiguration: [qsutils.makeQueryShapeConfiguration(querySettingsB, queryB)]
               });
           });
    });

    describe("query settings removal", function() {
        it("fails when the query settings entry for a different query shape has been deleted concurrently.",
           function() {
               runQuerySettingsCommandsConcurrently({
                   initialConfiguration: [
                       qsutils.makeQueryShapeConfiguration(querySettingsB, queryA),
                       qsutils.makeQueryShapeConfiguration(querySettingsB, queryB)
                   ],
                   commandToHangOnFailpoint: qsutils.makeRemoveQuerySettingsCommand(queryB),
                   commandToPass: qsutils.makeRemoveQuerySettingsCommand(queryAInstance2),
                   finalConfiguration: [qsutils.makeQueryShapeConfiguration(querySettingsB, queryB)]
               });
           });

        it("succeeds but does not remove the representative query if setQuerySettings commmand has been issued right after",
           function() {
               // Do not run this test case in multiversion, because
               // 'pauseAfterCallingSetClusterParameterInQuerySettingsCommands' failpoint is not
               // defined on versions before 8.2.
               const isMultiversion =
                   Boolean(jsTest.options().useRandomBinVersionsWithinReplicaSet) ||
                   Boolean(TestData.multiversionBinVersion);
               if (isMultiversion) {
                   return;
               }

               runQuerySettingsCommandsConcurrently({
                   initialConfiguration: [
                       qsutils.makeQueryShapeConfiguration(querySettingsA, queryA),
                   ],
                   commandToHangOnFailpoint: qsutils.makeRemoveQuerySettingsCommand(queryA),
                   expectedHangedCommandStatusCode: 0,
                   commandToPass: qsutils.makeSetQuerySettingsCommand(
                       qsutils.makeQueryShapeConfiguration(querySettingsB, queryA)),
                   finalConfiguration:
                       [qsutils.makeQueryShapeConfiguration(querySettingsB, queryA)],
                   failpointConfiguration: {
                       name: "pauseAfterCallingSetClusterParameterInQuerySettingsCommands",
                       data: {hangOnCmdType: "remove"},
                   },
               });
           });
    });
});
