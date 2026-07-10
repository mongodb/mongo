/**
 * Tests that the 'maxTimeMS' query setting, set via QuerySettings, behaves correctly across FCV
 * upgrade and downgrade:
 *   - Once fully upgraded, 'maxTimeMS' can be set and read back via setQuerySettings.
 *   - While downgraded, setting 'maxTimeMS' is rejected because it is FCV-gated behind
 *     'featureFlagPqsMaxTimeMS'.
 *   - On FCV downgrade, 'maxTimeMS' is stripped from existing query settings, while the remaining
 *     settings are retained. A setting that contained only 'maxTimeMS' is removed entirely.
 *   - After a downgrade then re-upgrade, the stripped 'maxTimeMS' stays gone and a new one can be
 *     set again.
 **/
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {QuerySettingsUtils} from "jstests/libs/query/query_settings_utils.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const maxTimeMS = 5000;

function setFCV(db, version) {
    jsTest.log.info("Setting feature compatibility version", {version});
    assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: version, confirm: true}));
}

const deployments = [
    {
        name: "replica set",
        start: () => {
            const rst = new ReplSetTest({nodes: 2});
            rst.startSet();
            rst.initiate();
            return {fixture: rst, conn: rst.getPrimary()};
        },
        stop: (fixture) => fixture.stopSet(),
    },
    {
        name: "sharded cluster",
        start: () => {
            const st = new ShardingTest({shards: 1, rs: {nodes: 2}});
            return {fixture: st, conn: st.s};
        },
        stop: (fixture) => fixture.stop(),
    },
];

// 'maxTimeMS' is gated on a single FCV, but a downgrade can target either the last-LTS or
// last-continuous FCV; exercise both since they can disable different sets of features.
for (const deployment of deployments) {
    for (const downgradeFCV of [lastLTSFCV, lastContinuousFCV]) {
        describe(`QuerySettings maxTimeMS on ${deployment.name} downgrading to FCV ${downgradeFCV}`, function () {
            before(function () {
                jsTest.log.info("Starting deployment", {deployment: deployment.name});
                const {fixture, conn} = deployment.start();
                this.fixture = fixture;
                this.db = conn.getDB(jsTestName());
                this.coll = assertDropAndRecreateCollection(this.db, jsTestName());
                this.qsutils = new QuerySettingsUtils(this.db, this.coll.getName());

                // Define the starting configuration with maxTimeMS. 'maxTimeMSOnlyQuery' carries
                // only 'maxTimeMS', so stripping it leaves it empty and it should be removed
                // entirely. 'comboQuery' carries 'maxTimeMS' alongside 'reject', so stripping
                // should retain 'reject' while removing only 'maxTimeMS'.
                this.comboQuery = this.qsutils.makeFindQueryInstance({filter: {a: 1}});
                this.maxTimeMSOnlyQuery = this.qsutils.makeFindQueryInstance({filter: {f: 1}});
                this.startingConfiguration = [
                    [{reject: true, maxTimeMS}, this.comboQuery],
                    [{maxTimeMS}, this.maxTimeMSOnlyQuery],
                ].map((args) => this.qsutils.makeQueryShapeConfiguration(...args));

                // Define the expected configuration after downgrade. maxTimeMS is stripped, and
                // 'maxTimeMSOnlyQuery' disappears since it would otherwise be empty.
                this.strippedConfiguration = [[{reject: true}, this.comboQuery]].map((args) =>
                    this.qsutils.makeQueryShapeConfiguration(...args),
                );

                // Install the starting configuration at the latest FCV.
                jsTest.log.info("Installing starting query settings configuration", {
                    startingConfiguration: this.startingConfiguration,
                });
                for (const config of this.startingConfiguration) {
                    assert.commandWorked(
                        this.db.adminCommand(this.qsutils.makeSetQuerySettingsCommand(config)),
                    );
                }
                this.qsutils.assertQueryShapeConfiguration(this.startingConfiguration);
            });

            after(function () {
                jsTest.log.info("Tearing down deployment", {deployment: deployment.name});
                deployment.stop(this.fixture);
            });

            describe("On downgrade", function () {
                before(function () {
                    setFCV(this.db, downgradeFCV);
                });

                it("Should strip maxTimeMS from existing settings", function () {
                    jsTest.log.info("Asserting maxTimeMS was stripped after downgrade", {
                        expected: this.strippedConfiguration,
                    });
                    this.qsutils.assertQueryShapeConfiguration(this.strippedConfiguration);
                });

                it("Should reject new maxTimeMS from being added", function () {
                    const setQuerySettings = this.qsutils.makeSetQuerySettingsCommand({
                        representativeQuery: this.comboQuery,
                        settings: {maxTimeMS},
                    });
                    jsTest.log.info(
                        "Asserting setQuerySettings with maxTimeMS is rejected while downgraded",
                        {downgradeFCV, setQuerySettings},
                    );
                    assert.commandFailed(this.db.adminCommand(setQuerySettings));
                });
            });

            describe("On re-upgrade", function () {
                before(function () {
                    setFCV(this.db, latestFCV);
                });

                it("Should not have the stripped maxTimeMS reappear", function () {
                    jsTest.log.info(
                        "Asserting stripped maxTimeMS did not reappear after re-upgrade",
                        {expected: this.strippedConfiguration},
                    );
                    this.qsutils.assertQueryShapeConfiguration(this.strippedConfiguration);
                });

                it("Should allow setting new maxTimeMS", function () {
                    // Set maxTimeMS on a fresh query shape and confirm it once again surfaces in
                    // the explain output.
                    const newQuery = this.qsutils.makeFindQueryInstance({filter: {g: 1}});
                    const settings = {maxTimeMS};
                    jsTest.log.info("Setting new maxTimeMS after re-upgrade", {
                        newQuery,
                        settings,
                    });
                    this.qsutils.withQuerySettings(newQuery, settings, () => {
                        this.qsutils.assertExplainQuerySettings(newQuery, settings);
                    });
                });
            });
        });
    }
}
