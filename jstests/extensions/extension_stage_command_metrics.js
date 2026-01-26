/**
 * Tests that successful and unsuccessful aggregate commands which use extension stages
 * are tracked in serverStatus.metrics.commands.aggregate.extensions counters.
 *
 * The expected behavior is:
 * - Aggregate commands that successfully complete AND use at least one extension stage should
 *   increment the 'extensions.succeeded' counter.
 * - Aggregate commands that fail AND use at least one extension stage should increment the
 *   'extensions.failed' counter.
 * - Aggregate commands that do NOT use any extension stages should NOT affect either counter.
 * - Extension stages that are ONLY in view definitions (not in the user's pipeline or its
 *   sub-pipelines) should NOT contribute to extension usage metrics.
 *   TODO SERVER-117646 Count usage in views?
 * - Extension stages in sub-pipelines of the user's pipeline (e.g., $unionWith, $lookup, $facet)
 *   SHOULD contribute to extension usage metrics.
 *
 * This test is written in TDD (Test-Driven Development) style to describe the expected behavior.
 * The implementation should check the '_usedExtensions' flag in ExtensionMetrics before
 * incrementing counters, to avoid tracking non-extension aggregate commands.
 *
 * Metrics path:
 * - serverStatus.metrics.commands.aggregate.extensions.succeeded
 * - serverStatus.metrics.commands.aggregate.extensions.failed
 *
 * @tags: [featureFlagExtensionsAPI]
 */
import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";

const collName = jsTestName();

// The 'serverStatus' command is unreliable in test suites with multiple mongos processesgiven that
// each node has its own metrics. The assertions here would not hold up if run against multiple
// mongos.
TestData.pinToSingleMongos = true;

/**
 * Helper function to get the extension-specific metrics from serverStatus.
 * Returns an object with {succeeded, failed} counters for aggregate commands using extensions.
 *
 * The metrics are stored at:
 * - serverStatus.metrics.commands.aggregate.withExtension.succeeded
 * - serverStatus.metrics.commands.aggregate.withExtension.failed
 */
function getExtensionCommandMetrics() {
    const serverStatus = assert.commandWorked(db.adminCommand({serverStatus: 1}));
    return serverStatus.metrics.commands.aggregate.withExtension;
}

function observeExtensionMetricsChange(pipelinesToRun) {
    const metricsBefore = getExtensionCommandMetrics();
    let nSuccesses = 0,
        nFailures = 0;
    for (const {coll, pipeline} of pipelinesToRun) {
        try {
            coll.aggregate(pipeline);
            nSuccesses++;
        } catch (e) {
            nFailures++;
        }
    }
    const metricsAfter = getExtensionCommandMetrics();
    return {
        nSuccessfulPipelines: nSuccesses,
        successMetricDelta: metricsAfter.succeeded - metricsBefore.succeeded,
        nFailedPipelines: nFailures,
        failureMetricDelta: metricsAfter.failed - metricsBefore.failed,
    };
}

/**
 * Verifies that the extension command metrics are changed by the provided pipelines.
 */
function verifyExtensionMetricsChange(pipelinesToRun) {
    const {nSuccessfulPipelines, successMetricDelta, nFailedPipelines, failureMetricDelta} =
        observeExtensionMetricsChange(pipelinesToRun);
    assert.eq(successMetricDelta, nSuccessfulPipelines);
    assert.eq(failureMetricDelta, nFailedPipelines);
}

/**
 * Verifies that the extension command metrics are *not* changed by the provided pipelines.
 */
function verifyExtensionMetricsDoNotChange(pipelinesToRun) {
    const {nSuccessfulPipelines, successMetricDelta, nFailedPipelines, failureMetricDelta} =
        observeExtensionMetricsChange(pipelinesToRun);
    assert.eq(successMetricDelta, 0);
    assert.eq(failureMetricDelta, 0);
}

describe("Extension stage command metrics", function () {
    let coll;

    before(function () {
        coll = db[collName];
    });

    beforeEach(function () {
        coll.drop();
        assert.commandWorked(coll.insert([{counter: 1}, {counter: 2}, {counter: 3}]));
    });

    it("should increment 'succeeded' for successful aggregate with extension stage", function () {
        verifyExtensionMetricsChange([
            {
                coll,
                pipeline: [{$metrics: {}}],
            },
        ]);
    });

    it("should increment 'failed' for failed aggregate with extension stage", function () {
        verifyExtensionMetricsChange([
            {
                coll,
                pipeline: [{$assert: {errmsg: "intentional failure for test", code: 12345, assertionType: "uassert"}}],
            },
        ]);
    });

    it("should NOT affect counters for successful aggregate WITHOUT extension stages", function () {
        verifyExtensionMetricsDoNotChange([
            {
                coll,
                pipeline: [{$match: {counter: {$gte: 1}}}],
            },
        ]);
    });

    it("should NOT affect counters for failed aggregate WITHOUT extension stages", function () {
        verifyExtensionMetricsDoNotChange([
            {
                coll,
                pipeline: [{$group: {_id: "$foo", total: {$invalidOperator: 1}}}],
            },
        ]);
    });

    it("should increment 'succeeded' for mixed pipeline with extension stage", function () {
        verifyExtensionMetricsChange([
            {
                coll,
                pipeline: [{$match: {counter: {$gte: 1}}}, {$metrics: {}}, {$limit: 10}],
            },
        ]);
    });

    it("should accumulate counters correctly for multiple aggregate commands", function () {
        const failingPipeline = [
            {
                $assert: {
                    errmsg: "intentional failure",
                    code: 12346,
                    assertionType: "uassert",
                },
            },
        ];
        verifyExtensionMetricsChange([
            {
                coll,
                pipeline: [{$metrics: {}}],
            },
            {
                coll,
                pipeline: failingPipeline,
            },
            {
                coll,
                pipeline: failingPipeline,
            },
        ]);
    });

    it("should count multiple extension stages in one pipeline as one command", function () {
        verifyExtensionMetricsChange([
            {
                coll,
                pipeline: [{$metrics: {}}, {$metrics: {}}, {$metrics: {}}],
            },
        ]);
    });

    describe("View edge cases", function () {
        const viewName = jsTestName() + "_view";
        const nestedViewName = jsTestName() + "_nested_view";

        beforeEach(function () {
            db[viewName].drop();
            db[nestedViewName].drop();
        });

        after(function () {
            // Clean up views after tests.
            db[viewName].drop();
            db[nestedViewName].drop();
        });

        function createView(pipeline) {
            assert.commandWorked(db.createView(viewName, collName, pipeline));
            return db[viewName];
        }
        function createNestedView(pipeline) {
            assert.commandWorked(db.createView(nestedViewName, viewName, pipeline));
            return db[nestedViewName];
        }

        it("should NOT increment counters when extension stage is ONLY in view definition", function () {
            // Create a view with an extension stage in its pipeline.
            const view = createView([{$testFoo: {}}, {$addFields: {fromView: true}}]);

            verifyExtensionMetricsDoNotChange([
                {
                    coll: view,
                    pipeline: [{$match: {counter: {$gte: 1}}}],
                },
            ]);
        });

        it("should increment 'succeeded' when extension stage is used ON a view", function () {
            // Create a regular view without extension stages.
            const view = createView([{$addFields: {fromView: true}}]);
            verifyExtensionMetricsChange([
                {
                    coll: view,
                    pipeline: [{$metrics: {}}],
                },
            ]);
        });

        it("should NOT increment counters for nested views with extension stages ONLY in view definitions", function () {
            // Create a base view with extension stage.
            createView([{$testFoo: {}}, {$addFields: {level: 1}}]);
            // Create a nested view on top of the first view with another extension stage.
            const nestedView = createNestedView([{$testFoo: {}}, {$addFields: {level: 2}}]);

            verifyExtensionMetricsDoNotChange([
                {
                    coll: nestedView,
                    pipeline: [{$match: {counter: {$gte: 1}}}],
                },
            ]);
        });

        it("should NOT affect counters for view WITHOUT extension stages", function () {
            const view = createView([{$addFields: {fromView: true}}]);

            verifyExtensionMetricsDoNotChange([
                {
                    coll: view,
                    pipeline: [{$match: {counter: {$gte: 1}}}],
                },
            ]);
        });
    });

    describe("Sub-pipeline edge cases", function () {
        const otherCollName = jsTestName() + "_other";
        let otherColl;

        before(function () {
            otherColl = db[otherCollName];
        });

        beforeEach(function () {
            otherColl.drop();
            assert.commandWorked(otherColl.insert([{value: 10}, {value: 20}]));
        });

        it("should increment 'succeeded' for extension stage in $unionWith sub-pipeline", function () {
            verifyExtensionMetricsChange([
                {
                    coll,
                    pipeline: [
                        {
                            $unionWith: {
                                coll: otherCollName,
                                pipeline: [{$toast: {temp: 300.0, numSlices: 1}}],
                            },
                        },
                    ],
                },
            ]);
        });

        it("should increment 'failed' for extension stage in $lookup sub-pipeline (rejected)", function () {
            verifyExtensionMetricsChange([
                {
                    coll,
                    pipeline: [
                        {
                            $lookup: {
                                from: otherCollName,
                                as: "joined",
                                pipeline: [{$testFoo: {}}],
                            },
                        },
                    ],
                },
            ]);
        });

        it("should increment 'failed' for extension stage in $facet sub-pipeline (rejected)", function () {
            verifyExtensionMetricsChange([
                {
                    coll,
                    pipeline: [{$facet: {facetOutput: [{$testFoo: {}}]}}],
                },
            ]);
        });

        it("should increment 'succeeded' for nested $unionWith with extension stages", function () {
            verifyExtensionMetricsChange([
                {
                    coll,
                    pipeline: [
                        {
                            $unionWith: {
                                coll: otherCollName,
                                pipeline: [
                                    {
                                        $unionWith: {
                                            coll: collName,
                                            pipeline: [{$extensionLimit: 1}],
                                        },
                                    },
                                ],
                            },
                        },
                    ],
                },
            ]);
        });

        it("should NOT affect counters for $lookup WITHOUT extension stages", function () {
            verifyExtensionMetricsDoNotChange([
                {
                    coll: coll,
                    pipeline: [
                        {
                            $lookup: {
                                from: otherCollName,
                                as: "joined",
                                pipeline: [{$match: {value: {$gte: 10}}}],
                            },
                        },
                    ],
                },
            ]);
        });

        it("should NOT affect counters for $facet WITHOUT extension stages", function () {
            verifyExtensionMetricsDoNotChange([
                {coll: coll, pipeline: [{$facet: {output: [{$match: {counter: {$gte: 1}}}]}}]},
            ]);
        });
    });

    describe("Combined scenarios", function () {
        const viewName = jsTestName() + "_combined_view";
        const otherCollName = jsTestName() + "_combined_other";

        beforeEach(function () {
            db[viewName].drop();
            db[otherCollName].drop();
            assert.commandWorked(db[otherCollName].insert([{x: 1}]));
        });

        after(function () {
            db[viewName].drop();
            db[otherCollName].drop();
        });

        function createView(pipeline) {
            assert.commandWorked(db.createView(viewName, collName, pipeline));
            return db[viewName];
        }

        it("should increment 'succeeded' for extension stage in user pipeline (view also has extension)", function () {
            const view = createView([{$testFoo: {}}]);
            verifyExtensionMetricsChange([
                {
                    coll: view,
                    pipeline: [{$metrics: {}}],
                },
            ]);
        });

        it("should NOT increment counters for view with $unionWith containing extension stage (extension is in view, not user pipeline)", function () {
            const view = createView([
                {
                    $unionWith: {
                        coll: otherCollName,
                        pipeline: [{$toast: {temp: 200.0, numSlices: 1}}],
                    },
                },
            ]);

            verifyExtensionMetricsDoNotChange([
                {
                    coll: view,
                    pipeline: [{$match: {}}],
                },
            ]);
        });
    });
});
