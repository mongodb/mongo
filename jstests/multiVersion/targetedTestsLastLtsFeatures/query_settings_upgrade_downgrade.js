/**
 * Tests that the 'representativeQueries' are migrated to the dedicated collection on FCV upgrade
 * and back to 'querySettings' cluster parameter on FCV downgrade.
 *
 * @tags: [featureFlagPQSBackfill]
 **/
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {describe, it} from "jstests/libs/mochalite.js";
import {QuerySettingsUtils} from "jstests/libs/query/query_settings_utils.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    testPerformUpgradeDowngradeReplSet
} from "jstests/multiVersion/libs/mixed_version_fixture_test.js";
import {
    testPerformUpgradeDowngradeSharded
} from "jstests/multiVersion/libs/mixed_version_sharded_fixture_test.js";

const dbName = "test";
const collName = jsTestName();
const getDB = (primaryConnection) => primaryConnection.getDB(jsTestName());

describe("QuerySettings", function() {
    describe(
        "should migrate 'representativeQueries' to the dedicated collection on FCV upgrade and migrate it back to 'querySettings' cluster parameter on FCV downgrade",
        function() {
            let qsutils;
            let queryA;
            let queryB;
            const exampleQuerySettings = {reject: true};

            const setupFn = function(conn) {
                const db = getDB(conn);
                const coll = assertDropAndRecreateCollection(db, collName);
                qsutils = new QuerySettingsUtils(db, coll.getName());
                qsutils.removeAllQuerySettings();

                queryA = qsutils.makeFindQueryInstance({filter: {a: 15}});
                queryB = qsutils.makeFindQueryInstance({filter: {b: 15}});

                assert.commandWorked(
                    db.adminCommand({setQuerySettings: queryA, settings: exampleQuerySettings}));
                assert.commandWorked(
                    db.adminCommand({setQuerySettings: queryB, settings: exampleQuerySettings}));
            };

            const assertQueryShapeConfigurations = function(isFullyUpgraded) {
                return function(conn) {
                    qsutils.assertQueryShapeConfiguration([
                        qsutils.makeQueryShapeConfiguration(exampleQuerySettings, queryA),
                        qsutils.makeQueryShapeConfiguration(exampleQuerySettings, queryB)
                    ]);

                    // Ensure that the 'representativeQueries' are migrated to the dedicated
                    // collection.
                    const isBackfillEnabled =
                        FeatureFlagUtil.isPresentAndEnabled(getDB(conn).getMongo(), 'PQSBackfill');
                    const expectedRepresentativeQueries =
                        isFullyUpgraded && isBackfillEnabled ? [queryA, queryB] : [];
                    qsutils.assertRepresentativeQueries(expectedRepresentativeQueries);
                };
            };

            const assertQueryShapeConfigurationsWithEmptyRepresentativeQueries =
                assertQueryShapeConfigurations(false);
            const assertQueryShapeConfigurationsWithMigratedRepresentativeQueries =
                assertQueryShapeConfigurations(true);

            it("in replica set", function() {
                testPerformUpgradeDowngradeReplSet({
                    setupFn,
                    whenFullyDowngraded:
                        assertQueryShapeConfigurationsWithEmptyRepresentativeQueries,
                    whenSecondariesAreLatestBinary:
                        assertQueryShapeConfigurationsWithEmptyRepresentativeQueries,
                    whenBinariesAreLatestAndFCVIsLastLTS:
                        assertQueryShapeConfigurationsWithEmptyRepresentativeQueries,
                    whenFullyUpgraded:
                        assertQueryShapeConfigurationsWithMigratedRepresentativeQueries,
                });
            });

            it("in sharded cluster", function() {
                testPerformUpgradeDowngradeSharded({
                    setupFn,
                    whenFullyDowngraded:
                        assertQueryShapeConfigurationsWithEmptyRepresentativeQueries,
                    whenOnlyConfigIsLatestBinary:
                        assertQueryShapeConfigurationsWithEmptyRepresentativeQueries,
                    whenSecondariesAndConfigAreLatestBinary:
                        assertQueryShapeConfigurationsWithEmptyRepresentativeQueries,
                    whenMongosBinaryIsLastLTS:
                        assertQueryShapeConfigurationsWithEmptyRepresentativeQueries,
                    whenBinariesAreLatestAndFCVIsLastLTS:
                        assertQueryShapeConfigurationsWithEmptyRepresentativeQueries,
                    whenFullyUpgraded:
                        assertQueryShapeConfigurationsWithMigratedRepresentativeQueries,
                });
            });
        });

    describe(
        "should migrate as many 'representativeQuery's as possible on FCV downgrade and not fail with BSONObjectTooLarge exception",
        function() {
            function runTest(db) {
                const qsutils = new QuerySettingsUtils(db, collName);
                const queryA =
                    qsutils.makeFindQueryInstance({filter: {a: "a".repeat(9 * 1024 * 1024)}});
                const queryB =
                    qsutils.makeFindQueryInstance({filter: {b: "b".repeat(10 * 1024 * 1024)}});
                for (const queryInstance of [queryA, queryB]) {
                    assert.commandWorked(db.adminCommand(
                        {setQuerySettings: queryInstance, settings: {reject: true}}));
                }

                // Perform FCV downgrade to the last LTS version, which should migrate
                // representativeQueries back to the 'querySettings' cluster parameter.
                assert.commandWorked(
                    db.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));

                const expectedQuerySettings = [
                    {
                        "settings": {"reject": true},
                        "representativeQuery": {
                            "find": "query_settings_upgrade_downgrade",
                            "$db": "test",
                            "filter": {"a": "a".repeat(9 * 1024 * 1024)}
                        }
                    },
                    {"settings": {"reject": true}},
                ];

                // Wrap the assertion into assert.soon as mongos needs some time to refresh
                // 'querySettings' cluster parameter value.
                assert.soonNoExcept(() => {
                    assert.sameMembers(qsutils.getQuerySettings(), expectedQuerySettings);
                    return true;
                });

                // Clean up all query settings.
                qsutils.removeAllQuerySettings();
            }

            it("in replica set", function() {
                const rst = new ReplSetTest({nodes: 2, nodeOptions: {binVersion: 'latest'}});
                rst.startSet();
                rst.initiate();

                const db = rst.getPrimary().getDB(dbName);
                try {
                    runTest(db);
                } finally {
                    rst.stopSet();
                }
            });

            it("in sharded cluster", function() {
                const st = new ShardingTest({shards: 1, mongos: 1, config: 1, rs: {nodes: 2}});
                const db = st.s.getDB(dbName);
                try {
                    runTest(db);
                } finally {
                    st.stop();
                }
            });
        });

    describe(
        "should perform FCV upgrade and downgrade successully after first failed attempt due to repeated failure of setClusterParamater command",
        function() {
            function runTest(db) {
                // Ensure we are on the last LTS FCV before starting the test.
                assert.commandWorked(
                    db.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));

                const qsutils = new QuerySettingsUtils(db, collName);
                const queryA = qsutils.makeFindQueryInstance({filter: {a: 1}});
                const queryB = qsutils.makeFindQueryInstance({filter: {b: "string"}});
                for (const queryInstance of [queryA, queryB]) {
                    assert.commandWorked(db.adminCommand(
                        {setQuerySettings: queryInstance, settings: {reject: true}}));
                }
                qsutils.assertQueryShapeConfiguration(
                    [
                        qsutils.makeQueryShapeConfiguration({reject: true}, queryA),
                        qsutils.makeQueryShapeConfiguration({reject: true}, queryB)
                    ],
                    false /* shouldRunExplain */);

                // Attempt to upgrade the FCV, which should fail due to the failpoint.
                qsutils.withFailpoint(
                    "throwConflictingOperationInProgressOnQuerySettingsSetClusterParameter",
                    {},
                    () => {
                        assert.commandFailedWithCode(
                            db.adminCommand(
                                {setFeatureCompatibilityVersion: latestFCV, confirm: true}),
                            ErrorCodes.TemporarilyUnavailable);
                    });

                // Run FCV upgrade command again, which should succeed now.
                assert.commandWorked(
                    db.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));

                // Attempt to downgrade the FCV, which should fail due to the failpoint.
                qsutils.withFailpoint(
                    "throwConflictingOperationInProgressOnQuerySettingsSetClusterParameter",
                    {},
                    () => {
                        assert.commandFailedWithCode(
                            db.adminCommand(
                                {setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
                            ErrorCodes.TemporarilyUnavailable);
                    });

                // Run FCV downgrade command again, which should succeed now.
                assert.commandWorked(
                    db.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));

                // Clean up all query settings.
                qsutils.removeAllQuerySettings();
            }

            it("in replica set", function() {
                const rst = new ReplSetTest({nodes: 2, nodeOptions: {binVersion: 'latest'}});
                rst.startSet();
                rst.initiate();

                const db = rst.getPrimary().getDB(dbName);
                try {
                    runTest(db);
                } finally {
                    rst.stopSet();
                }
            });

            it("in sharded cluster", function() {
                const st = new ShardingTest({shards: 1, mongos: 1, config: 1, rs: {nodes: 2}});
                const db = st.s.getDB(dbName);
                try {
                    runTest(db);
                } finally {
                    st.stop();
                }
            });
        });
});
