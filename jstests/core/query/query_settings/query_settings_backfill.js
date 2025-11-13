// Test that query settings set by hash have their representative queries backfilled on the
// following execution.
// @tags: [
//   requires_fcv_83,
//   # TODO SERVER-98659 Investigate why this test is failing on
//   # 'sharding_kill_stepdown_terminate_jscore_passthrough'.
//   does_not_support_stepdowns,
//   # This test asserts on the backfill server status section, which needs to be ran against the
//   # same host that executed the queries.
//   assumes_read_preference_unchanged,
//   not_allowed_with_signed_security_token,
//   simulate_mongoq_incompatible,
//   directly_against_shardsvrs_incompatible,
//   simulate_atlas_proxy_incompatible,
//   # TODO SERVER-89461 Investigate why test using huge batch size timeout in suites with balancer.
//   assumes_balancer_off,
//   # TODO(SERVER-113800): Enable setClusterParameters with replicaset started with --shardsvr
//   transitioning_replicaset_incompatible,
// ]

import {kGenericArgFieldNames} from "jstests/libs/cmd_object_utils.js";
import {assertDropAndRecreateCollection, assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {QuerySettingsUtils} from "jstests/libs/query/query_settings_utils.js";

class QuerySettingsBackfillMetricsTests {
    constructor(qsutils) {
        this.qsutils = qsutils;
        this.capturedMetrics = this.getBackfillMetrics();
    }

    getBackfillMetrics() {
        return this.qsutils.getQuerySettingsServerStatus().backfill;
    }

    verify(...fns) {
        const currentMetrics = this.getBackfillMetrics();
        fns.forEach((fn) => assert.doesNotThrow(() => fn(currentMetrics)), "Backfill metrics validation failed");
    }

    captureCurrentMetrics() {
        this.capturedMetrics = this.getBackfillMetrics();
    }

    missingRepresentativeQueriesIs(expected) {
        return ({missingRepresentativeQueries}) =>
            assert.eq(
                missingRepresentativeQueries,
                expected,
                "Expected 'missingRepresentativeQueries' to be " +
                    expected +
                    " but found " +
                    missingRepresentativeQueries,
            );
    }

    bufferedRepresentativeQueriesIs(expected) {
        return ({bufferedRepresentativeQueries}) =>
            assert.eq(
                bufferedRepresentativeQueries,
                expected,
                "Expected 'bufferedRepresentativeQueries' to be " +
                    expected +
                    " but found " +
                    bufferedRepresentativeQueries,
            );
    }

    memoryUsedBytesIs(expected) {
        return ({memoryUsedBytes}) =>
            assert.eq(
                memoryUsedBytes,
                expected,
                "Expected memory used bytes to be " + expected + " but found " + memoryUsedBytes,
            );
    }

    memoryUsedBytesIncreased() {
        return ({memoryUsedBytes}) =>
            assert.gt(memoryUsedBytes, this.capturedMetrics.memoryUsedBytes, "Expected memory used to increase");
    }

    memoryUsedBytesDidNotIncrease() {
        return this.memoryUsedBytesIs(this.capturedMetrics.memoryUsedBytes);
    }

    insertedRepresentativeQueriesIncreasedBy(increment) {
        return ({insertedRepresentativeQueries}) =>
            assert.eq(
                insertedRepresentativeQueries,
                this.capturedMetrics.insertedRepresentativeQueries + increment,
                "Expected the 'insertedRepresentativeQueries' count to increase by " + increment,
            );
    }

    succeededBackfillsIncreasedBy(increment) {
        return ({succeededBackfills}) =>
            assert.eq(
                succeededBackfills,
                this.capturedMetrics.succeededBackfills + increment,
                "Expected the 'succeededBackfills ' count to increase by " + increment,
            );
    }

    failedBackfillsIncreasedBy(increment) {
        return ({failedBackfills}) =>
            assert.eq(
                failedBackfills,
                this.capturedMetrics.failedBackfills + increment,
                "Expected the 'failedBackfills' count to increase by " + increment,
            );
    }
}

describe("QuerySettingsBackfill", function () {
    let qsutils = null;
    let metrics = null;
    let coll = null;
    let view = null;

    function runQueryAndExpectBackfill({query, settings}) {
        const queryShapeHash = qsutils.getQueryShapeHashFromExplain(query);
        const cmdObj = qsutils.withoutDollarDB(query);

        // There might be pending backfills from previous tests. Assert that they'll eventually
        // finish, so we can start the backfill assertions from a clean state.
        assert.soonNoExcept(() => {
            metrics.verify(metrics.missingRepresentativeQueriesIs(0), metrics.memoryUsedBytesIs(0));
            return true;
        });

        function runTest() {
            // Capture the current metrics and set query settings before starting the test.
            metrics.captureCurrentMetrics();
            // Set the query settings by hash for the given 'query'.
            qsutils.withQuerySettings(queryShapeHash, settings, () => {
                // Assert that the representative query is not present in $querySettings yet.
                qsutils.assertQueryShapeConfiguration([{settings}], /* shouldRunExplain */ false);

                // Assert that there is now one missing representative queries.
                metrics.verify(
                    metrics.missingRepresentativeQueriesIs(1),
                    metrics.bufferedRepresentativeQueriesIs(0),
                    metrics.memoryUsedBytesIs(0),
                );

                // Assert that no backfill specific metric was increased yet.
                metrics.verify(
                    metrics.succeededBackfillsIncreasedBy(0),
                    metrics.failedBackfillsIncreasedBy(0),
                    metrics.insertedRepresentativeQueriesIncreasedBy(0),
                );

                // Configure a failpoint to block the backfill task execution.
                const fp = configureFailPoint(db, "hangBeforeExecutingBackfillTask");

                // Execute the query and ensure that it is buffered. Expect the memory used to
                // increase.
                metrics.captureCurrentMetrics();
                assert.commandWorked(db.runCommand(cmdObj));
                metrics.verify(metrics.bufferedRepresentativeQueriesIs(1), metrics.memoryUsedBytesIncreased());

                // Execute the same query again and ensure that it isn't buffered again and no
                // additional memory is used.
                metrics.captureCurrentMetrics();
                assert.commandWorked(db.runCommand(cmdObj));
                metrics.verify(metrics.bufferedRepresentativeQueriesIs(1), metrics.memoryUsedBytesDidNotIncrease());

                // Unblock the execution of the task.
                fp.off();

                // Assert that the representative query will soon be present in $querySettings.
                const expectedConfiguration = [qsutils.makeQueryShapeConfiguration(settings, query)];
                qsutils.assertQueryShapeConfiguration(
                    expectedConfiguration,
                    /* shouldRunExplain */ true,
                    /* ignoreRepresentativeQueryFields */ kGenericArgFieldNames,
                );

                // Expect the task to have finished succesfully. Both the success metrics and the
                // number of inserted representative queries should have increased, while the fail
                // count should remain constant.
                metrics.verify(
                    metrics.succeededBackfillsIncreasedBy(1),
                    metrics.failedBackfillsIncreasedBy(0),
                    metrics.insertedRepresentativeQueriesIncreasedBy(1),
                );

                // Assert that there is now no missing representative queries and no memory used.
                metrics.verify(
                    metrics.missingRepresentativeQueriesIs(0),
                    metrics.bufferedRepresentativeQueriesIs(0),
                    metrics.memoryUsedBytesIs(0),
                );
            });
            return true;
        }
        qsutils.withBackfillDelaySeconds(1, () => assert.soonNoExcept(runTest));
    }

    // Create a collection and a view before the tests run.
    before(function () {
        coll = assertDropAndRecreateCollection(db, jsTestName());
        const viewName = coll.getName() + "_view";
        assertDropCollection(db, viewName);
        assert.commandWorked(db.createView(viewName, coll.getName(), []));
        view = db[viewName];
    });

    // Drop both the collection and the view after the tests are done.
    after(function () {
        assertDropCollection(db, view.getName());
        assertDropCollection(db, coll.getName());
    });

    describe("Against Regular Collections", function () {
        before(function () {
            qsutils = new QuerySettingsUtils(db, coll.getName());
            metrics = new QuerySettingsBackfillMetricsTests(qsutils);
            qsutils.removeAllQuerySettings();
        });

        it("Should backfill find commands", function () {
            runQueryAndExpectBackfill({
                qsutils,
                query: qsutils.makeFindQueryInstance({filter: {a: 1}}),
                settings: {queryFramework: "classic"},
            });
        });

        it("Should backfill find commands with let vars", function () {
            runQueryAndExpectBackfill({
                qsutils,
                query: qsutils.makeFindQueryInstance({filter: {a: "$b"}, let: {b: 1}}),
                settings: {queryFramework: "classic"},
            });
        });

        it("Should backfill distinct commands", function () {
            runQueryAndExpectBackfill({
                qsutils,
                query: qsutils.makeDistinctQueryInstance({key: "a", query: {b: 1}}),
                settings: {queryFramework: "classic"},
            });
        });

        it("Should backfill simple aggregate commands", function () {
            runQueryAndExpectBackfill({
                qsutils,
                query: qsutils.makeAggregateQueryInstance({pipeline: [{$match: {a: 1}}]}),
                settings: {queryFramework: "classic"},
            });
        });

        it("Should backfill aggregate commands with let vars", function () {
            runQueryAndExpectBackfill({
                qsutils,
                query: qsutils.makeAggregateQueryInstance({
                    pipeline: [{$match: {a: "$b"}}],
                    let: {b: 1},
                }),
                settings: {queryFramework: "classic"},
            });
        });

        it("Should backfill aggregate commands with $lookup", function () {
            runQueryAndExpectBackfill({
                qsutils,
                query: qsutils.makeAggregateQueryInstance({
                    pipeline: [
                        {
                            $lookup: {
                                from: coll.getName(),
                                as: "c",
                                pipeline: [{$match: {a: 1}}],
                            },
                        },
                    ],
                }),
                settings: {queryFramework: "classic"},
            });
        });

        it("Should backfill change stream commands", function () {
            if (TestData.isTimeseriesTestSuite) {
                // The timeseries override is incompatible with change streams.
                return;
            }
            runQueryAndExpectBackfill({
                qsutils,
                query: qsutils.makeAggregateQueryInstance({
                    pipeline: [{$changeStream: {}}],
                }),
                settings: {queryFramework: "classic"},
            });
        });
    });

    describe("Against Views", function () {
        before(function () {
            qsutils = new QuerySettingsUtils(db, coll.getName());
            metrics = new QuerySettingsBackfillMetricsTests(qsutils);
            qsutils.removeAllQuerySettings();
        });

        it("Should backfill simple find commands", function () {
            runQueryAndExpectBackfill({
                qsutils,
                query: qsutils.makeFindQueryInstance({filter: {a: 1}}),
                settings: {queryFramework: "classic"},
            });
        });

        it("Should backfill distinct commands", function () {
            runQueryAndExpectBackfill({
                qsutils,
                query: qsutils.makeDistinctQueryInstance({key: "a", query: {b: 1}}),
                settings: {queryFramework: "classic"},
            });
        });

        it("Should backfill simple aggregate commands", function () {
            runQueryAndExpectBackfill({
                qsutils,
                query: qsutils.makeAggregateQueryInstance({pipeline: [{$match: {a: 1}}]}),
                settings: {queryFramework: "classic"},
            });
        });
    });

    describe("Against non-existent collections", function () {
        before(function () {
            qsutils = new QuerySettingsUtils(db, "nonExistentCollection");
            metrics = new QuerySettingsBackfillMetricsTests(qsutils);
            qsutils.removeAllQuerySettings();
        });

        it("Should backfill simple find commands", function () {
            runQueryAndExpectBackfill({
                qsutils,
                query: qsutils.makeFindQueryInstance({filter: {a: 1}}),
                settings: {queryFramework: "classic"},
            });
        });

        it("Should backfill distinct commands", function () {
            runQueryAndExpectBackfill({
                qsutils,
                query: qsutils.makeDistinctQueryInstance({key: "a", query: {b: 1}}),
                settings: {queryFramework: "classic"},
            });
        });

        it("Should backfill simple aggregate commands", function () {
            runQueryAndExpectBackfill({
                qsutils,
                query: qsutils.makeAggregateQueryInstance({pipeline: [{$match: {a: 1}}]}),
                settings: {queryFramework: "classic"},
            });
        });

        it("Should backfill collectionless aggregate commands", function () {
            runQueryAndExpectBackfill({
                qsutils,
                query: qsutils.makeAggregateQueryInstance(
                    {pipeline: [{$querySettings: {}}]},
                    /* collectionless */ true,
                ),
                settings: {queryFramework: "classic"},
            });
        });
    });
});
