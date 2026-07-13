/**
 * Tests that 'pqs_settable' query knobs set via QuerySettings behave correctly across FCV upgrade
 * and downgrade:
 *   - Once fully upgraded, 'queryKnobs' can be set and read back via setQuerySettings.
 *   - While downgraded, setting 'queryKnobs' is rejected because the feature is FCV-gated behind
 *     'featureFlagPqsQueryKnobs'.
 *   - On FCV downgrade, 'queryKnobs' are stripped from existing query settings, while the remaining
 *     settings are retained. Settings that contained only 'queryKnobs' are removed entirely.
 *   - After a downgrade then re-upgrade, the stripped 'queryKnobs' stay gone and new 'queryKnobs'
 *     can be set again.
 *
 * @tags: [featureFlagPqsQueryKnobs]
 **/
import {getExplainCommand} from "jstests/libs/cmd_object_utils.js";
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {QuerySettingsUtils} from "jstests/libs/query/query_settings_utils.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const findKnobs = {samplingMarginOfError: 3.0, cbrCEMode: "histogramCE"};
const aggKnobs = {samplingConfidenceInterval: "99", samplingCEMethod: "random"};

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

// 'queryKnobs' are gated on a single FCV, but a downgrade can target either the last-LTS or
// last-continuous FCV; exercise both since they can disable different sets of features.
for (const deployment of deployments) {
    for (const downgradeFCV of [lastLTSFCV, lastContinuousFCV]) {
        describe(`QuerySettings queryKnobs on ${deployment.name} downgrading to FCV ${downgradeFCV}`, function () {
            before(function () {
                jsTest.log.info("Starting deployment", {deployment: deployment.name});
                const {fixture, conn} = deployment.start();
                this.fixture = fixture;
                this.db = conn.getDB(jsTestName());
                this.coll = assertDropAndRecreateCollection(this.db, jsTestName());
                this.qsutils = new QuerySettingsUtils(this.db, this.coll.getName());

                // Define the starting configuration with query knobs. 'knobsOnlyQuery' carries only
                // 'queryKnobs', so stripping them leaves it empty and it should be removed entirely.
                this.findQuery = this.qsutils.makeFindQueryInstance({filter: {a: 1}});
                this.aggQuery = this.qsutils.makeAggregateQueryInstance({
                    pipeline: [{$match: {b: 1}}],
                });
                this.distinctQuery = this.qsutils.makeDistinctQueryInstance({key: "c"});
                this.knobsOnlyQuery = this.qsutils.makeFindQueryInstance({filter: {e: 1}});
                this.startingConfiguration = [
                    [{queryFramework: "classic", queryKnobs: findKnobs}, this.findQuery],
                    [{queryFramework: "sbe", queryKnobs: aggKnobs}, this.aggQuery],
                    [{reject: true}, this.distinctQuery],
                    [{queryKnobs: findKnobs}, this.knobsOnlyQuery],
                ].map((args) => this.qsutils.makeQueryShapeConfiguration(...args));

                // Define the expected configuration after downgrade. Query knobs are stripped, and
                // 'knobsOnlyQuery' disappears since it would otherwise be empty.
                this.strippedConfiguration = [
                    [{queryFramework: "classic"}, this.findQuery],
                    [{queryFramework: "sbe"}, this.aggQuery],
                    [{reject: true}, this.distinctQuery],
                ].map((args) => this.qsutils.makeQueryShapeConfiguration(...args));

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

                it("Should strip query knobs from existing settings", function () {
                    jsTest.log.info("Asserting query knobs were stripped after downgrade", {
                        expected: this.strippedConfiguration,
                    });
                    this.qsutils.assertQueryShapeConfiguration(this.strippedConfiguration);
                });

                it("Should reject new query knobs from being added", function () {
                    const setQuerySettings = this.qsutils.makeSetQuerySettingsCommand({
                        representativeQuery: this.distinctQuery,
                        settings: {queryKnobs: findKnobs},
                    });
                    jsTest.log.info(
                        "Asserting setQuerySettings with query knobs is rejected while downgraded",
                        {downgradeFCV, setQuerySettings},
                    );
                    assert.commandFailed(this.db.adminCommand(setQuerySettings));
                });
            });

            describe("On re-upgrade", function () {
                before(function () {
                    setFCV(this.db, latestFCV);
                });

                it("Should not have the stripped query knobs reappear", function () {
                    jsTest.log.info(
                        "Asserting stripped query knobs did not reappear after re-upgrade",
                        {expected: this.strippedConfiguration},
                    );
                    this.qsutils.assertQueryShapeConfiguration(this.strippedConfiguration);
                });

                it("Should allow setting new knobs", function () {
                    // Set query knobs on a fresh query shape and confirm they once again surface in
                    // the explain output.
                    const newQuery = this.qsutils.makeFindQueryInstance({filter: {d: 1}});
                    const settings = {queryKnobs: findKnobs};
                    jsTest.log.info("Setting new query knobs after re-upgrade", {
                        newQuery,
                        settings,
                    });
                    this.qsutils.withQuerySettings(newQuery, settings, () => {
                        const explainCmd = getExplainCommand(
                            this.qsutils.withoutDollarDB(newQuery),
                        );
                        const explain = assert.commandWorked(this.db.runCommand(explainCmd));
                        const explainKnobs = explain.queryKnobs ?? {};
                        for (const [wireName, value] of Object.entries(findKnobs)) {
                            assert.eq(
                                explainKnobs[wireName]?.value,
                                value,
                                "query knob missing in explain",
                                {wireName, explainKnobs},
                            );
                        }
                    });
                });
            });
        });
    }
}
