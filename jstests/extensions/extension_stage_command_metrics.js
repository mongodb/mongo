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

/**
 * Verifies that the extension command metrics changed by the expected amounts.
 */
function verifyExtensionMetricsChange({metricsBefore, metricsAfter, expectedSuccessChange, expectedFailedChange}) {
    assert.eq(
        metricsBefore.succeeded + expectedSuccessChange,
        metricsAfter.succeeded,
        `Expected 'succeeded' to increase by ${expectedSuccessChange}, ` +
            `but got ${metricsAfter.succeeded - metricsBefore.succeeded}`,
    );
    assert.eq(
        metricsBefore.failed + expectedFailedChange,
        metricsAfter.failed,
        `Expected 'failed' to increase by ${expectedFailedChange}, ` +
            `but got ${metricsAfter.failed - metricsBefore.failed}`,
    );
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
        const metricsBefore = getExtensionCommandMetrics();

        // Run a successful aggregate with the $metrics extension stage.
        assert.commandWorked(
            db.runCommand({
                aggregate: collName,
                pipeline: [{$metrics: {}}],
                cursor: {},
            }),
        );

        verifyExtensionMetricsChange({
            metricsBefore,
            metricsAfter: getExtensionCommandMetrics(),
            expectedSuccessChange: 1,
            expectedFailedChange: 0,
        });
    });

    it("should increment 'failed' for failed aggregate with extension stage", function () {
        const metricsBefore = getExtensionCommandMetrics();

        // Run a failing aggregate using the $assert extension stage.
        assert.commandFailed(
            db.runCommand({
                aggregate: collName,
                pipeline: [
                    {
                        $assert: {
                            errmsg: "intentional failure for test",
                            code: 12345,
                            assertionType: "uassert",
                        },
                    },
                ],
                cursor: {},
            }),
        );

        verifyExtensionMetricsChange({
            metricsBefore,
            metricsAfter: getExtensionCommandMetrics(),
            expectedSuccessChange: 0,
            expectedFailedChange: 1,
        });
    });

    it("should NOT affect counters for successful aggregate WITHOUT extension stages", function () {
        const metricsBefore = getExtensionCommandMetrics();

        // Run a successful aggregate with only built-in stages (no extensions).
        assert.commandWorked(
            db.runCommand({
                aggregate: collName,
                pipeline: [{$match: {counter: {$gte: 1}}}],
                cursor: {},
            }),
        );

        verifyExtensionMetricsChange({
            metricsBefore,
            metricsAfter: getExtensionCommandMetrics(),
            expectedSuccessChange: 0,
            expectedFailedChange: 0,
        });
    });

    it("should NOT affect counters for failed aggregate WITHOUT extension stages", function () {
        const metricsBefore = getExtensionCommandMetrics();

        // Run a failing aggregate with only built-in stages (invalid operator).
        assert.commandFailed(
            db.runCommand({
                aggregate: collName,
                pipeline: [{$group: {_id: "$foo", total: {$invalidOperator: 1}}}],
                cursor: {},
            }),
        );

        verifyExtensionMetricsChange({
            metricsBefore,
            metricsAfter: getExtensionCommandMetrics(),
            expectedSuccessChange: 0,
            expectedFailedChange: 0,
        });
    });

    it("should increment 'succeeded' for mixed pipeline with extension stage", function () {
        const metricsBefore = getExtensionCommandMetrics();

        // Run a successful aggregate with both built-in and extension stages.
        assert.commandWorked(
            db.runCommand({
                aggregate: collName,
                pipeline: [{$match: {counter: {$gte: 1}}}, {$metrics: {}}, {$limit: 10}],
                cursor: {},
            }),
        );

        verifyExtensionMetricsChange({
            metricsBefore,
            metricsAfter: getExtensionCommandMetrics(),
            expectedSuccessChange: 1,
            expectedFailedChange: 0,
        });
    });

    it("should accumulate counters correctly for multiple aggregate commands", function () {
        const metricsBefore = getExtensionCommandMetrics();

        // Run 3 successful aggregates with extension stages.
        for (let i = 0; i < 3; i++) {
            assert.commandWorked(
                db.runCommand({
                    aggregate: collName,
                    pipeline: [{$metrics: {}}],
                    cursor: {},
                }),
            );
        }

        // Run 2 failing aggregates with extension stages.
        for (let i = 0; i < 2; i++) {
            assert.commandFailed(
                db.runCommand({
                    aggregate: collName,
                    pipeline: [
                        {
                            $assert: {
                                errmsg: "intentional failure " + i,
                                code: 12346,
                                assertionType: "uassert",
                            },
                        },
                    ],
                    cursor: {},
                }),
            );
        }

        verifyExtensionMetricsChange({
            metricsBefore,
            metricsAfter: getExtensionCommandMetrics(),
            expectedSuccessChange: 3,
            expectedFailedChange: 2,
        });
    });

    it("should count multiple extension stages in one pipeline as one command", function () {
        const metricsBefore = getExtensionCommandMetrics();

        // Run an aggregate with multiple extension stages.
        assert.commandWorked(
            db.runCommand({
                aggregate: collName,
                pipeline: [{$metrics: {}}, {$metrics: {}}, {$metrics: {}}],
                cursor: {},
            }),
        );

        // Should only increment by 1 even though there are 3 extension stages.
        verifyExtensionMetricsChange({
            metricsBefore,
            metricsAfter: getExtensionCommandMetrics(),
            expectedSuccessChange: 1,
            expectedFailedChange: 0,
        });
    });
});
