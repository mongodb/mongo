// Test that query settings set by hash have their representative queries backfilled on the
// following execution.
// @tags: [
//   requires_fcv_82,
//   featureFlagPQSBackfill,
//   # TODO SERVER-98659 Investigate why this test is failing on
//   # 'sharding_kill_stepdown_terminate_jscore_passthrough'.
//   does_not_support_stepdowns,
//   not_allowed_with_signed_security_token,
//   simulate_mongoq_incompatible,
//   directly_against_shardsvrs_incompatible,
//   simulate_atlas_proxy_incompatible,
//   # TODO SERVER-89461 Investigate why test using huge batch size timeout in suites with balancer.
//   assumes_balancer_off,
// ]

import {kGenericArgFieldNames} from "jstests/libs/cmd_object_utils.js";
import {
    assertDropAndRecreateCollection,
    assertDropCollection
} from "jstests/libs/collection_drop_recreate.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {QuerySettingsUtils} from "jstests/libs/query/query_settings_utils.js";

describe("QuerySettingsBackfill", function() {
    let qsutils = null;
    let coll = null;
    let view = null;

    function runQueryAndExpectBackfill({query, settings}) {
        // Set the query settings by hash for the given 'query'.
        const queryShapeHash = qsutils.getQueryShapeHashFromExplain(query);
        const cmdObj = qsutils.withoutDollarDB(query);
        function assertQueryWasBackfilled() {
            // Execute the query so it has a chance to be backfilled.
            assert.commandWorked(db.runCommand(cmdObj));

            // Assert that the representative query will soon be present in $querySettings.
            const expectedConfiguration = [qsutils.makeQueryShapeConfiguration(settings, query)];
            qsutils.assertQueryShapeConfiguration(
                expectedConfiguration,
                /* shouldRunExplain */ true,
                /* ignoreRepresentativeQueryFields */ kGenericArgFieldNames);
            return true;
        };
        qsutils.withBackfillDelaySeconds(1, () => {
            qsutils.withQuerySettings(queryShapeHash, settings, () => {
                // Assert that the representative query is not present in $querySettings yet.
                qsutils.assertQueryShapeConfiguration([{settings}], /* shouldRunExplain */ false);
                // Representative queries are not guaranteed to be backfilled immediately and might
                // require additional executions, so wrap the test in assert.soonNoExcept() to
                // ensure it eventually passes.
                assert.soonNoExcept(assertQueryWasBackfilled);
            });
        });
    }

    // Create a collection and a view before the tests run.
    before(function() {
        coll = assertDropAndRecreateCollection(db, jsTestName());
        const viewName = coll.getName() + "_view";
        assertDropCollection(db, viewName);
        assert.commandWorked(db.createView(viewName, coll.getName(), []));
        view = db[viewName];
    });

    // Drop both the collection and the view after the tests are done.
    after(function() {
        assertDropCollection(db, view.getName());
        assertDropCollection(db, coll.getName());
    });

    describe("Against Regular Collections", function() {
        before(function() {
            qsutils = new QuerySettingsUtils(db, coll.getName());
            qsutils.removeAllQuerySettings();
        });

        it("Should backfill find commands", function() {
            runQueryAndExpectBackfill({
                qsutils,
                query: qsutils.makeFindQueryInstance({filter: {a: 1}}),
                settings: {queryFramework: "classic"}
            });
        });

        it("Should backfill find commands with let vars", function() {
            runQueryAndExpectBackfill({
                qsutils,
                query: qsutils.makeFindQueryInstance({filter: {a: '$b'}, let : {b: 1}}),
                settings: {queryFramework: "classic"}
            });
        });

        it("Should backfill distinct commands", function() {
            runQueryAndExpectBackfill({
                qsutils,
                query: qsutils.makeDistinctQueryInstance({key: "a", query: {b: 1}}),
                settings: {queryFramework: "classic"}
            });
        });

        it("Should backfill simple aggregate commands", function() {
            runQueryAndExpectBackfill({
                qsutils,
                query: qsutils.makeAggregateQueryInstance({pipeline: [{$match: {a: 1}}]}),
                settings: {queryFramework: "classic"}
            });
        });

        it("Should backfill aggregate commands with let vars", function() {
            runQueryAndExpectBackfill({
                qsutils,
                query: qsutils.makeAggregateQueryInstance({
                    pipeline: [{$match: {a: '$b'}}],
                    let : {b: 1},
                }),
                settings: {queryFramework: "classic"}
            });
        });

        it("Should backfill aggregate commands with $lookup", function() {
            runQueryAndExpectBackfill({
                qsutils,
                query: qsutils.makeAggregateQueryInstance({
                    pipeline: [{
                        $lookup: {
                            from: coll.getName(),
                            as: "c",
                            pipeline: [{$match: {a: 1}}],
                        }
                    }],
                }),
                settings: {queryFramework: "classic"}
            });
        });

        it("Should backfill change stream commands", function() {
            if (TestData.isTimeseriesTestSuite) {
                // The timeseries override is incompatible with change streams.
                return;
            }
            runQueryAndExpectBackfill({
                qsutils,
                query: qsutils.makeAggregateQueryInstance({
                    pipeline: [{$changeStream: {}}],
                }),
                settings: {queryFramework: "classic"}
            });
        });
    });

    describe("Against Views", function() {
        before(function() {
            qsutils = new QuerySettingsUtils(db, coll.getName());
            qsutils.removeAllQuerySettings();
        });

        it("Should backfill simple find commands", function() {
            runQueryAndExpectBackfill({
                qsutils,
                query: qsutils.makeFindQueryInstance({filter: {a: 1}}),
                settings: {queryFramework: "classic"}
            });
        });

        it("Should backfill distinct commands", function() {
            runQueryAndExpectBackfill({
                qsutils,
                query: qsutils.makeDistinctQueryInstance({key: "a", query: {b: 1}}),
                settings: {queryFramework: "classic"}
            });
        });

        it("Should backfill simple aggregate commands", function() {
            runQueryAndExpectBackfill({
                qsutils,
                query: qsutils.makeAggregateQueryInstance({pipeline: [{$match: {a: 1}}]}),
                settings: {queryFramework: "classic"}
            });
        });
    });

    describe("Against non-existent collections", function() {
        before(function() {
            qsutils = new QuerySettingsUtils(db, "nonExistentCollection");
            qsutils.removeAllQuerySettings();
        });

        it("Should backfill simple find commands", function() {
            runQueryAndExpectBackfill({
                qsutils,
                query: qsutils.makeFindQueryInstance({filter: {a: 1}}),
                settings: {queryFramework: "classic"}
            });
        });

        it("Should backfill distinct commands", function() {
            runQueryAndExpectBackfill({
                qsutils,
                query: qsutils.makeDistinctQueryInstance({key: "a", query: {b: 1}}),
                settings: {queryFramework: "classic"}
            });
        });

        it("Should backfill simple aggregate commands", function() {
            runQueryAndExpectBackfill({
                qsutils,
                query: qsutils.makeAggregateQueryInstance({pipeline: [{$match: {a: 1}}]}),
                settings: {queryFramework: "classic"}
            });
        });

        it("Should backfill collectionless aggregate commands", function() {
            runQueryAndExpectBackfill({
                qsutils,
                query: qsutils.makeAggregateQueryInstance({pipeline: [{$querySettings: {}}]},
                                                          /* collectionless */ true),
                settings: {queryFramework: "classic"}
            });
        });
    });
});
