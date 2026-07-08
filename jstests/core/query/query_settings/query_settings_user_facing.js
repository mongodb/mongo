/**
 * Tests that querySettings can be passed directly in commands as runtime hints when the
 * featureFlagAllowUserFacingQuerySettings is enabled, and is rejected when disabled.
 * When enabled, verifies that the settings are actually applied by checking explain output,
 * and that cluster PQS takes precedence over user-supplied settings on conflict.
 * @tags: [
 *   does_not_support_stepdowns,
 *   directly_against_shardsvrs_incompatible,
 *   simulate_atlas_proxy_incompatible,
 *   # TODO(SERVER-113800): Enable setClusterParameters with replicaset started with --shardsvr
 *   transitioning_replicaset_incompatible,
 *   requires_fcv_90,
 *   # Uses runWithParamsAllNonConfigNodes which requires a stable shard list.
 *   assumes_stable_shard_list,
 * ]
 */
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {describe, it} from "jstests/libs/mochalite.js";
import {getEngine} from "jstests/libs/query/analyze_plan.js";
import {QuerySettingsIndexHintsTests} from "jstests/libs/query/query_settings_index_hints_tests.js";
import {QuerySettingsUtils} from "jstests/libs/query/query_settings_utils.js";
import {runWithParamsAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

const coll = assertDropAndRecreateCollection(db, jsTestName());
assert.commandWorked(
    coll.insert([
        {a: 1, b: 2},
        {a: 2, b: 1},
    ]),
);
assert.commandWorked(coll.createIndexes([{a: 1}, {b: 1}]));

const ns = {db: db.getName(), coll: coll.getName()};

const querySettings = {
    indexHints: {ns, allowedIndexes: [{a: 1}]},
};

const flagEnabled = FeatureFlagUtil.isPresentAndEnabled(
    db.getMongo(),
    "AllowUserFacingQuerySettings",
);

const qsutils = new QuerySettingsUtils(db, coll.getName());
const qstests = new QuerySettingsIndexHintsTests(qsutils);

/**
 * Asserts that the given command uses the expected engine via explain.
 * Skipped on timeseries collections which always use classic.
 */
function assertEngine(cmd, expectedEngine) {
    if (TestData.isTimeseriesTestSuite) {
        return;
    }
    const explain = assert.commandWorked(db.runCommand({explain: cmd}));
    assert.eq(getEngine(explain), expectedEngine, "Expected " + expectedEngine + " engine");
}

describe("User-facing querySettings when flag is off", function () {
    if (flagEnabled) {
        jsTest.log.info(
            "Skipping flag-off tests: featureFlagAllowUserFacingQuerySettings is enabled",
        );
        return;
    }

    it("should reject querySettings on find", function () {
        assert.commandFailedWithCode(
            db.runCommand({find: coll.getName(), filter: {a: 1}, querySettings}),
            [7923000, 7746900],
        );
    });

    it("should reject querySettings on aggregate", function () {
        assert.commandFailedWithCode(
            db.runCommand({
                aggregate: coll.getName(),
                pipeline: [{$match: {a: 1}}],
                cursor: {},
                querySettings,
            }),
            [7923000],
        );
    });

    it("should reject querySettings on distinct", function () {
        assert.commandFailedWithCode(
            db.runCommand({distinct: coll.getName(), key: "a", querySettings}),
            [7923001, 7923000],
        );
    });
});

describe("User-facing querySettings when flag is on", function () {
    if (!flagEnabled) {
        jsTest.log.info(
            "Skipping flag-on tests: featureFlagAllowUserFacingQuerySettings is disabled",
        );
        return;
    }

    it("should accept querySettings on find, aggregate, and distinct", function () {
        assert.commandWorked(db.runCommand({find: coll.getName(), filter: {a: 1}, querySettings}));
        assert.commandWorked(
            db.runCommand({
                aggregate: coll.getName(),
                pipeline: [{$match: {a: 1}}],
                cursor: {},
                querySettings,
            }),
        );
        assert.commandWorked(db.runCommand({distinct: coll.getName(), key: "a", querySettings}));
    });

    it("should apply index hints to find via explain", function () {
        const cmd = {find: coll.getName(), filter: {a: 1, b: 1}, querySettings};
        qstests.assertIndexScanStage(cmd, {a: 1}, ns);
    });

    it("should apply index hints to aggregate via explain", function () {
        const cmd = {
            aggregate: coll.getName(),
            pipeline: [{$match: {a: 1, b: 1}}],
            cursor: {},
            querySettings,
        };
        qstests.assertIndexScanStage(cmd, {a: 1}, ns);
    });

    // Timeseries passthrough rewrites distinct to an aggregate pipeline, which produces
    // different plan stages (UNPACK_TS_BUCKET + COLLSCAN instead of DISTINCT_SCAN).
    if (!TestData.isTimeseriesTestSuite) {
        it("should apply index hints to distinct via explain", function () {
            const cmd = {distinct: coll.getName(), key: "a", query: {a: {$gt: 0}}, querySettings};
            qstests.assertDistinctScanStage(cmd, {a: 1});
        });
    }

    // Unlike setQuerySettings, empty/default user settings on a query command are a no-op, not an
    // error.
    it("accepts empty settings as a no-op", function () {
        assert.commandWorked(
            db.runCommand({find: coll.getName(), filter: {a: 1}, querySettings: {}}),
        );
    });

    it("accepts settings that simplify to default as a no-op", function () {
        assert.commandWorked(
            db.runCommand({find: coll.getName(), filter: {a: 1}, querySettings: {reject: false}}),
        );
    });

    it("rejects two index hints targeting the same collection", function () {
        assert.commandFailedWithCode(
            db.runCommand({
                find: coll.getName(),
                filter: {a: 1},
                querySettings: {
                    indexHints: [
                        {ns, allowedIndexes: [{a: 1}]},
                        {ns, allowedIndexes: [{b: 1}]},
                    ],
                },
            }),
            [7746608],
        );
    });

    it("rejects an index hint with an empty key pattern", function () {
        assert.commandFailedWithCode(
            db.runCommand({
                find: coll.getName(),
                filter: {a: 1},
                querySettings: {indexHints: {ns, allowedIndexes: [{}]}},
            }),
            [9646000, 9646001],
        );
    });

    // The 'queryKnobs' field is itself gated behind featureFlagPqsQueryKnobs. When that flag is
    // disabled, passing it inline must be rejected by the shared validation.
    const knobsFlagEnabled = FeatureFlagUtil.isPresentAndEnabled(db.getMongo(), "PqsQueryKnobs");
    if (!knobsFlagEnabled) {
        it("rejects queryKnobs when featureFlagPqsQueryKnobs is disabled", function () {
            assert.commandFailedWithCode(
                db.runCommand({
                    find: coll.getName(),
                    filter: {a: 1},
                    querySettings: {queryKnobs: {samplingMarginOfError: 3.0}},
                }),
                [12324800],
            );
        });
    }

    // The 'maxTimeMS' field is itself gated behind featureFlagPqsMaxTimeMS. When that flag is
    // disabled, passing it inline must be rejected by the shared validation.
    const maxTimeMSFlagEnabled = FeatureFlagUtil.isPresentAndEnabled(db.getMongo(), "PqsMaxTimeMS");
    if (!maxTimeMSFlagEnabled) {
        it("rejects maxTimeMS when featureFlagPqsMaxTimeMS is disabled", function () {
            assert.commandFailedWithCode(
                db.runCommand({
                    find: coll.getName(),
                    filter: {a: 1},
                    querySettings: {maxTimeMS: NumberLong(5000)},
                }),
                [12998200],
            );
        });
    }
});

// Queries that are ineligible for query settings (IDHACK/Express, FLE, internal/system
// namespaces) must ignore client-side settings entirely, neither validating nor applying them.
describe("Client-side querySettings on ineligible queries are ignored", function () {
    if (!flagEnabled) {
        return;
    }

    // Settings that would be rejected on an eligible query (duplicate index hints for the same
    // collection -> 7746608). On an ineligible query the command must still succeed, proving the
    // settings were skipped rather than validated/applied.
    const ineligibleSettings = {
        indexHints: [
            {ns, allowedIndexes: [{a: 1}]},
            {ns, allowedIndexes: [{b: 1}]},
        ],
    };

    it("ignores settings on IDHACK queries", function () {
        // A point predicate on {_id} is an IDHACK/Express query, ineligible for query settings.
        assert.commandWorked(
            db.runCommand({
                find: coll.getName(),
                filter: {_id: 1},
                querySettings: ineligibleSettings,
            }),
        );
    });

    it("ignores settings on internal-database / system-collection queries", function () {
        // 'admin.system.version' lives on an internal database and is a system collection, so it
        // is ineligible for query settings.
        assert.commandWorked(
            db.getSiblingDB("admin").runCommand({
                find: "system.version",
                filter: {},
                querySettings: ineligibleSettings,
            }),
        );
    });

    it("ignores settings on FLE queries", function () {
        // A command carrying 'encryptionInformation' is an FLE query, ineligible for query
        // settings.
        const encryptionInformation = {
            type: 1,
            schema: {
                [ns.db + "." + ns.coll]: {
                    escCollection: "enxcol_." + coll.getName() + ".esc",
                    ecocCollection: "enxcol_." + coll.getName() + ".ecoc",
                    fields: [
                        {
                            path: "secret",
                            keyId: UUID("11d58b8a-0c6c-4d69-a0bd-70c6d9befae9"),
                            bsonType: "string",
                            queries: {queryType: "equality"},
                        },
                    ],
                },
            },
        };
        assert.commandWorked(
            db.runCommand({
                find: coll.getName(),
                filter: {a: 1},
                encryptionInformation,
                querySettings: ineligibleSettings,
            }),
        );
    });
});

// Merge tests: cluster PQS wins on conflict, user settings fill gaps.
// These use withQuerySettings to set cluster PQS, then pass querySettings inline.
describe("Merging cluster PQS with user-supplied querySettings", function () {
    if (!flagEnabled) {
        return;
    }

    describe("find", function () {
        it("cluster PQS wins on index hint conflict", function () {
            const findQuery = qsutils.makeFindQueryInstance({filter: {a: 1, b: 1}});
            const clusterSettings = {indexHints: {ns, allowedIndexes: [{b: 1}]}};
            const userSettings = {indexHints: {ns, allowedIndexes: [{a: 1}]}};
            qsutils.withQuerySettings(findQuery, clusterSettings, () => {
                const cmd = {
                    find: coll.getName(),
                    filter: {a: 1, b: 1},
                    querySettings: userSettings,
                };
                qstests.assertIndexScanStage(cmd, {b: 1}, ns);
            });
        });

        it("user settings fill gaps when no cluster conflict", function () {
            runWithParamsAllNonConfigNodes(
                db,
                {internalQueryFrameworkControl: "forceClassicEngine"},
                () => {
                    const findCmd = {find: coll.getName(), filter: {a: 1, b: 1}};
                    assertEngine(findCmd, "classic");

                    const findQuery = qsutils.makeFindQueryInstance({filter: {a: 1, b: 1}});
                    const clusterSettings = {queryFramework: "sbe"};
                    const userSettings = {indexHints: {ns, allowedIndexes: [{a: 1}]}};
                    qsutils.withQuerySettings(findQuery, clusterSettings, () => {
                        const cmd = {...findCmd, querySettings: userSettings};
                        qstests.assertIndexScanStage(cmd, {a: 1}, ns);
                        assertEngine(cmd, "sbe");
                    });
                },
            );
        });
    });

    describe("aggregate", function () {
        it("cluster PQS wins on index hint conflict", function () {
            const aggQuery = qsutils.makeAggregateQueryInstance({
                pipeline: [{$match: {a: 1, b: 1}}],
            });
            const clusterSettings = {indexHints: {ns, allowedIndexes: [{b: 1}]}};
            const userSettings = {indexHints: {ns, allowedIndexes: [{a: 1}]}};
            qsutils.withQuerySettings(aggQuery, clusterSettings, () => {
                const cmd = {
                    aggregate: coll.getName(),
                    pipeline: [{$match: {a: 1, b: 1}}],
                    cursor: {},
                    querySettings: userSettings,
                };
                qstests.assertIndexScanStage(cmd, {b: 1}, ns);
            });
        });

        it("user settings fill gaps when no cluster conflict", function () {
            runWithParamsAllNonConfigNodes(
                db,
                {internalQueryFrameworkControl: "forceClassicEngine"},
                () => {
                    const aggCmd = {
                        aggregate: coll.getName(),
                        pipeline: [{$match: {a: 1, b: 1}}],
                        cursor: {},
                    };
                    assertEngine(aggCmd, "classic");

                    const aggQuery = qsutils.makeAggregateQueryInstance({
                        pipeline: [{$match: {a: 1, b: 1}}],
                    });
                    const clusterSettings = {queryFramework: "sbe"};
                    const userSettings = {indexHints: {ns, allowedIndexes: [{a: 1}]}};
                    qsutils.withQuerySettings(aggQuery, clusterSettings, () => {
                        const cmd = {...aggCmd, querySettings: userSettings};
                        qstests.assertIndexScanStage(cmd, {a: 1}, ns);
                        assertEngine(cmd, "sbe");
                    });
                },
            );
        });
    });

    if (!TestData.isTimeseriesTestSuite) {
        describe("distinct", function () {
            it("cluster PQS wins on index hint conflict", function () {
                // Cluster forces {b:1}, user requests {a:1} -- cluster should win.
                const distinctQuery = qsutils.makeDistinctQueryInstance({
                    key: "a",
                    query: {a: 1, b: 1},
                });
                const clusterSettings = {indexHints: {ns, allowedIndexes: [{b: 1}]}};
                const userSettings = {indexHints: {ns, allowedIndexes: [{a: 1}]}};
                qsutils.withQuerySettings(distinctQuery, clusterSettings, () => {
                    const cmd = {
                        distinct: coll.getName(),
                        key: "a",
                        query: {a: 1, b: 1},
                        querySettings: userSettings,
                    };
                    qstests.assertIndexScanStage(cmd, {b: 1}, ns);
                });
            });

            it("user settings fill gaps when no cluster conflict", function () {
                const distinctCmd = {distinct: coll.getName(), key: "a", query: {a: {$gt: 0}}};

                // Baseline: without any settings, the query should use classic engine.
                const baselineExplain = assert.commandWorked(db.runCommand({explain: distinctCmd}));
                assert.eq(
                    getEngine(baselineExplain),
                    "classic",
                    "Expected classic engine as baseline",
                );

                const distinctQuery = qsutils.makeDistinctQueryInstance({
                    key: "a",
                    query: {a: {$gt: 0}},
                });
                const clusterSettings = {queryFramework: "sbe"};
                const userSettings = {indexHints: {ns, allowedIndexes: [{a: 1}]}};
                qsutils.withQuerySettings(distinctQuery, clusterSettings, () => {
                    const cmd = {...distinctCmd, querySettings: userSettings};

                    // Verify user's index hint is applied.
                    qstests.assertDistinctScanStage(cmd, {a: 1});

                    //NOTE: while sbe engine is hinted, it is not applied in favor of DISTINCT_SCAN.
                    const explain = assert.commandWorked(db.runCommand({explain: cmd}));
                    assert.eq(
                        getEngine(explain),
                        "classic",
                        "Expected classic engine from cluster PQS",
                    );
                });
            });
        });
    }
});
