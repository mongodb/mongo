/**
 * Tests that the query stats sampling rate can be overridden per-query via the 'queryStatsSampleRate'
 * query knob through Parameterized Query Settings (PQS).
 *
 * A per-query override takes precedence over the cluster-wide sampling configuration:
 *   - override 1.0 collects the shape even when the cluster-wide sample rate is 0,
 *   - override 0.0 suppresses the shape even when the cluster-wide sample rate is 1.0.
 *
 * @tags: [
 *   # PQS setQuerySettings runs against a replica set.
 *   requires_replication,
 *   # Incompatible with setParameter.
 *   does_not_support_stepdowns,
 *   requires_fcv_91,
 * ]
 */
import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {isFCVgte} from "jstests/libs/feature_compatibility_version.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {QuerySettingsUtils} from "jstests/libs/query/query_settings_utils.js";
import {getQueryStats} from "jstests/libs/query/query_stats_utils.js";

describe("queryStatsSampleRate query knob", () => {
    let rst, testdb, coll, qsutils, representativeQuery, pqsOverrideSupported;

    before(() => {
        rst = new ReplSetTest({
            nodes: 1,
            nodeOptions: {
                setParameter: {
                    internalQueryStatsSampleRate: 0,
                    internalQueryStatsWriteCmdSampleRate: 0,
                },
            },
        });
        rst.startSet();
        rst.initiate();

        const conn = rst.getPrimary();
        testdb = conn.getDB("test");
        coll = testdb[jsTestName()];
        coll.drop();
        for (let i = 0; i < 20; i++) {
            assert.commandWorked(coll.insert({foo: i % 2, bar: Math.floor(Math.random() * 3)}));
        }

        qsutils = new QuerySettingsUtils(testdb, jsTestName());

        // The sampling-rate PQS knob is only settable via query settings at FCV 9.1+. Fixtures
        // that run below the latest FCV (disagg fixtures run at LAST_CONTINUOUS_FCV) reject the
        // override by design, so the override subtests skip themselves there.
        pqsOverrideSupported = isFCVgte(conn, "9.1");

        // The representative query shape we override, as a plain command object, and the query
        // execution that matches it (the same shape) so PQS resolution applies the overrides.
        representativeQuery = qsutils.makeFindQueryInstance({filter: {foo: 1}, batchSize: 2});
        const runQuery = () => coll.find({foo: 1}).batchSize(2).toArray();

        // Baseline: with a cluster-wide sample rate of 0, nothing is collected.
        runQuery();
        assert.eq(
            getQueryStats(testdb, {collName: coll.getName()}).length,
            0,
            "expected no query stats with cluster-wide sample rate 0",
        );
    });

    after(() => {
        rst.stopSet();
    });

    // Skips the subtest when the node's FCV is below the sampling knob's minFcv.
    const itIfOverrideSupported = (title, fn) =>
        it(title, () => {
            if (pqsOverrideSupported) {
                fn();
            }
        });

    itIfOverrideSupported(
        "collects an overridden shape despite a cluster-wide sample rate of 0",
        () => {
            // A per-query override of queryStatsSampleRate: 1.0 must force collection of that shape
            // despite the cluster-wide sample rate being 0.
            qsutils.withQuerySettings(
                representativeQuery,
                {queryKnobs: {queryStatsSampleRate: 1.0}},
                () => {
                    coll.find({foo: 1}).batchSize(2).toArray();
                },
            );
            const statsWithOverride = getQueryStats(testdb, {collName: coll.getName()});
            assert.eq(
                statsWithOverride.length,
                1,
                "expected the overridden shape to be collected despite cluster-wide sample rate 0",
            );
        },
    );

    itIfOverrideSupported(
        "suppresses an overridden shape despite a cluster-wide sample rate of 1",
        () => {
            // Raise the cluster-wide sample rate to 1.0 (collect everything) and override a single
            // shape to 0.0. That shape must be suppressed while other shapes are collected.
            assert.commandWorked(
                testdb.adminCommand({setParameter: 1, internalQueryStatsSampleRate: 1}),
            );
            coll.find({foo: 1}).batchSize(2).toArray();

            const statsBeforeInverse = getQueryStats(testdb, {collName: coll.getName()});
            assert.eq(
                statsBeforeInverse.length,
                1,
                "expected only the overridden shape before applying the 0.0 override",
            );
            const execCountBeforeInverse = statsBeforeInverse[0].metrics.execCount;
            const rateLimitedBefore =
                testdb.serverStatus().metrics.queryStats.numRateLimitedRequests;

            qsutils.withQuerySettings(
                representativeQuery,
                {queryKnobs: {queryStatsSampleRate: 0.0}},
                () => {
                    coll.find({foo: 1}).batchSize(2).toArray();
                },
            );

            const statsAfterInverse = getQueryStats(testdb, {collName: coll.getName()});
            assert.eq(
                statsAfterInverse.length,
                1,
                "expected only the non-overridden shape to remain",
                {
                    statsAfterInverse,
                },
            );
            // Prove the 0.0 override actually suppressed the request: execCount is unchanged from
            // before, rather than silently recollecting the already-present shape.
            assert.eq(
                statsAfterInverse[0].metrics.execCount,
                execCountBeforeInverse,
                "expected the 0.0 override to suppress collection of the overridden shape",
            );
            // The rejected request was suppressed solely by the override while query stats collection
            // remains enabled, so it counts as rate limited.
            assert.gt(
                testdb.serverStatus().metrics.queryStats.numRateLimitedRequests,
                rateLimitedBefore,
                "expected the rejected 0.0 override to count as rate limited",
            );
        },
    );

    describe("per-command querySettings", () => {
        beforeEach(() => {
            // A known cluster-wide rate of 0 for every case: collection can only come from an
            // override.
            assert.commandWorked(
                testdb.adminCommand({setParameter: 1, internalQueryStatsSampleRate: 0}),
            );
        });

        const execCountForShape = () => {
            const stats = getQueryStats(testdb, {collName: coll.getName()});
            return stats.length === 1 ? stats[0].metrics.execCount : 0;
        };

        // Run the representative shape as a raw command carrying a per-command querySettings
        // override, draining the returned cursor so the query stats entry is written (cursor-path
        // stats are deferred until cursor disposal). A shape-level PQS override is also active if
        // 'shapeSettings' is set.
        const runWithOverrides = ({shapeSettings, commandSettings}) => {
            const run = () => {
                const cmd = {find: coll.getName(), filter: {foo: 1}, batchSize: 2};
                if (commandSettings) {
                    cmd.querySettings = commandSettings;
                }
                const baseline = execCountForShape();
                let res = assert.commandWorked(testdb.runCommand(cmd));
                while (res.cursor.id > 0) {
                    res = assert.commandWorked(
                        testdb.runCommand({getMore: res.cursor.id, collection: coll.getName()}),
                    );
                }
                return {baseline, after: execCountForShape()};
            };

            if (shapeSettings) {
                return qsutils.withQuerySettings(representativeQuery, shapeSettings, run);
            }
            return run();
        };

        itIfOverrideSupported("collects when only the per-command override says 1.0", () => {
            const {baseline, after} = runWithOverrides({
                commandSettings: {queryKnobs: {queryStatsSampleRate: 1.0}},
            });
            assert.eq(after, baseline + 1, "expected the per-command override to force collection");
        });

        itIfOverrideSupported(
            "prefers the shape-level PQS override when the per-command override says 0.0",
            () => {
                // Conflicting overrides: the shape-level 1.0 wins the per-knob merge, so the query is
                // collected despite the per-command 0.0.
                const shapeSettings = {queryKnobs: {queryStatsSampleRate: 1.0}};
                const commandSettings = {queryKnobs: {queryStatsSampleRate: 0.0}};

                // Explain with the shape-level override active shows the resolved per-knob merge: the
                // shape-level value wins the conflict.
                qsutils.withQuerySettings(representativeQuery, shapeSettings, () => {
                    const explained = assert.commandWorked(
                        testdb.runCommand({
                            explain: {
                                find: coll.getName(),
                                filter: {foo: 1},
                                batchSize: 2,
                                querySettings: commandSettings,
                            },
                            verbosity: "queryPlanner",
                        }),
                    );
                    assert.docEq(
                        {queryStatsSampleRate: 1.0},
                        explained.queryPlanner.querySettings.queryKnobs,
                        "expected the shape-level PQS override to win the knob merge",
                        {resolved: explained.queryPlanner.querySettings},
                    );
                });

                const {baseline, after} = runWithOverrides({shapeSettings, commandSettings});
                assert.eq(
                    after,
                    baseline + 1,
                    "expected the shape-level 1.0 override to win and collect",
                );
            },
        );

        itIfOverrideSupported(
            "prefers the shape-level PQS override when the per-command override says 1.0",
            () => {
                // Conflicting overrides: the shape-level 0.0 wins the per-knob merge, so the query is
                // suppressed despite the per-command 1.0.
                const rateLimitedBefore =
                    testdb.serverStatus().metrics.queryStats.numRateLimitedRequests;
                const {baseline, after} = runWithOverrides({
                    shapeSettings: {queryKnobs: {queryStatsSampleRate: 0.0}},
                    commandSettings: {queryKnobs: {queryStatsSampleRate: 1.0}},
                });
                assert.eq(
                    after,
                    baseline,
                    "expected the shape-level 0.0 override to win and suppress",
                );
                // The rejected request was suppressed solely by the override while query stats
                // collection remains enabled, so it counts as rate limited.
                assert.gt(
                    testdb.serverStatus().metrics.queryStats.numRateLimitedRequests,
                    rateLimitedBefore,
                    "expected the rejected override to count as rate limited",
                );
            },
        );
    });
});
