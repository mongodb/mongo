/**
 * Tests that the 'executionControlConcurrencyAdjustmentAlgorithm' can be changed at runtime, and
 * that the server's behavior regarding ticket resizing adjusts accordingly.
 *
 * @tags: [
 *   # The test deploys replica sets with a execution control concurrency adjustment configured by
 *   # each test case, which should not be overwritten and expect to have 'throughputProbing' as
 *   # default.
 *   incompatible_with_execution_control_with_prioritization,
 * ]
 */

import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {
    getExecutionControlAlgorithm,
    getExecutionControlStats,
    kFixedConcurrentTransactionsAlgorithm,
    kThroughputProbingAlgorithm,
    setDeprioritizationGate,
    setExecutionControlAlgorithm,
    setExecutionControlTickets,
} from "jstests/noPassthrough/admission/execution_control/libs/execution_control_helper.js";

describe("Execution control concurrency adjustment algorithm", function () {
    // Global counters to track expected warning counts across all test calls
    let expectedDynamicWarnings = 0;
    let expectedPrioritizationWarnings = 0;

    /**
     * Helper to check for the presence of specific warning log messages.
     */
    function assertWarningLogs(mongod, logId, readParamName, writeParamName, expectedCount, warningType) {
        assert.soon(
            () => checkLog.checkContainsWithCountJson(mongod, logId, {"serverParameter": readParamName}, expectedCount),
            `Expected ${expectedCount} ${warningType} warning(s) for ${readParamName} not found`,
        );
        assert.soon(
            () =>
                checkLog.checkContainsWithCountJson(mongod, logId, {"serverParameter": writeParamName}, expectedCount),
            `Expected ${expectedCount} ${warningType} warning(s) for ${writeParamName} not found`,
        );
    }

    /**
     * Asserts that ticket resizing commands succeed. All ticket resizing operations are allowed
     * regardless of the algorithm, though warnings may be logged based on the current algorithm.
     */
    function assertTicketSizing(mongod, options = {}) {
        const expectDynamicWarnings = options.expectDynamicAdjustmentWarnings || false;
        const expectPrioritizationWarnings = options.expectPrioritizationWarnings || false;

        setExecutionControlTickets(mongod, {
            read: 25,
            write: 25,
            readLowPriority: 25,
            writeLowPriority: 25,
        });

        // Check for dynamic adjustment warnings (when throughput probing is active)
        if (expectDynamicWarnings) {
            expectedDynamicWarnings++;
            assertWarningLogs(
                mongod,
                11280900,
                "concurrent read transactions limit",
                "concurrent write transactions limit",
                expectedDynamicWarnings,
                "dynamic adjustment",
            );
        }

        // Check for prioritization warnings (when single pool without prioritization is used)
        if (expectPrioritizationWarnings) {
            expectedPrioritizationWarnings++;
            assertWarningLogs(
                mongod,
                11280901,
                "low priority concurrent read transactions limit",
                "low priority concurrent write transactions limit",
                expectedPrioritizationWarnings,
                "prioritization",
            );
        }
    }

    describe("Concurrency adjustment algorithm ticket re-sizing", function () {
        let replTest;

        beforeEach(function () {
            expectedDynamicWarnings = 0;
            expectedPrioritizationWarnings = 0;
        });

        afterEach(function () {
            if (replTest) {
                replTest.stopSet();
            }
        });

        it("should allow ticket resizing when throughput probing is enabled by default", function () {
            replTest = new ReplSetTest({nodes: 1});
            replTest.startSet();
            replTest.initiate();
            const mongod = replTest.getPrimary();

            assertTicketSizing(mongod, {expectDynamicAdjustmentWarnings: true, expectPrioritizationWarnings: true});
        });

        it("should allow ticket resizing when using the fixed concurrency adjustment algorithm", function () {
            replTest = new ReplSetTest({
                nodes: 1,
                nodeOptions: {
                    setParameter: {
                        executionControlConcurrencyAdjustmentAlgorithm: kFixedConcurrentTransactionsAlgorithm,
                    },
                },
            });
            replTest.startSet();
            replTest.initiate();
            const mongod = replTest.getPrimary();
            assertTicketSizing(mongod, {expectPrioritizationWarnings: true});
        });

        it("should implicitly use the fixed concurrency adjustment algorithm when tickets are set at startup", function () {
            replTest = new ReplSetTest({
                nodes: 1,
                nodeOptions: {setParameter: {executionControlConcurrentReadTransactions: 20}},
            });
            replTest.startSet();
            replTest.initiate();
            const mongod = replTest.getPrimary();

            assert.eq(kFixedConcurrentTransactionsAlgorithm, getExecutionControlAlgorithm(mongod));
        });

        it("should allow resizing all tickets with deprioritization enabled", function () {
            replTest = new ReplSetTest({
                nodes: 1,
                nodeOptions: {
                    setParameter: {
                        executionControlDeprioritizationGate: true,
                    },
                },
            });
            replTest.startSet();
            replTest.initiate();
            const mongod = replTest.getPrimary();
            assertTicketSizing(mongod, {expectDynamicAdjustmentWarnings: true});
        });
    });

    describe("Concurrency adjustment algorithm parameter validation", function () {
        let replTest, mongod;

        beforeEach(function () {
            replTest = new ReplSetTest({nodes: 1});
            replTest.startSet();
            replTest.initiate();
            mongod = replTest.getPrimary();
        });

        afterEach(function () {
            if (replTest) {
                replTest.stopSet();
            }
        });

        it("should fail to set an invalid algorithm name", function () {
            assert.commandFailedWithCode(
                mongod.adminCommand({
                    setParameter: 1,
                    executionControlConcurrencyAdjustmentAlgorithm: "invalidAlgorithm",
                }),
                ErrorCodes.BadValue,
            );
        });

        it("should succeed when setting the algorithm to its current value", function () {
            const initialAlgorithm = getExecutionControlAlgorithm(mongod);
            setExecutionControlAlgorithm(mongod, initialAlgorithm);
        });
    });

    describe("Concurrency adjustment algorithm and prioritization runtime transitions", function () {
        describe("From fixed concurrency adjustment algorithm behavior", function () {
            let replTest, mongod;

            beforeEach(function () {
                replTest = new ReplSetTest({
                    nodes: 1,
                    nodeOptions: {
                        setParameter: {
                            executionControlConcurrencyAdjustmentAlgorithm: kFixedConcurrentTransactionsAlgorithm,
                        },
                    },
                });
                replTest.startSet();
                replTest.initiate();
                mongod = replTest.getPrimary();

                expectedDynamicWarnings = 0;
                expectedPrioritizationWarnings = 0;
            });

            afterEach(function () {
                if (replTest) {
                    replTest.stopSet();
                }
            });

            it("should allow transitions to other algorithms and enabling/disabling prioritization", function () {
                assertTicketSizing(mongod, {expectPrioritizationWarnings: true});

                setDeprioritizationGate(mongod, true);
                assertTicketSizing(mongod);

                setExecutionControlAlgorithm(mongod, kThroughputProbingAlgorithm);
                assertTicketSizing(mongod, {expectDynamicAdjustmentWarnings: true});

                setExecutionControlAlgorithm(mongod, kFixedConcurrentTransactionsAlgorithm);
                assertTicketSizing(mongod);
            });
        });

        describe("From prioritization enabled behavior", function () {
            let replTest, mongod;

            beforeEach(function () {
                replTest = new ReplSetTest({
                    nodes: 1,
                    nodeOptions: {
                        setParameter: {
                            executionControlDeprioritizationGate: true,
                        },
                    },
                });
                replTest.startSet();
                replTest.initiate();
                mongod = replTest.getPrimary();

                expectedDynamicWarnings = 0;
                expectedPrioritizationWarnings = 0;
            });

            afterEach(function () {
                if (replTest) {
                    replTest.stopSet();
                }
            });

            it("should allow transitions to other algorithms and back", function () {
                setExecutionControlAlgorithm(mongod, kFixedConcurrentTransactionsAlgorithm);
                assertTicketSizing(mongod);

                setExecutionControlAlgorithm(mongod, kThroughputProbingAlgorithm);
                assertTicketSizing(mongod, {expectDynamicAdjustmentWarnings: true});

                setExecutionControlAlgorithm(mongod, kFixedConcurrentTransactionsAlgorithm);
                assertTicketSizing(mongod);
            });
        });

        describe("From throughput probing algorithm behavior", function () {
            let replTest, mongod;

            beforeEach(function () {
                replTest = new ReplSetTest({
                    nodes: 1,
                    nodeOptions: {
                        setParameter: {
                            executionControlConcurrencyAdjustmentAlgorithm: kThroughputProbingAlgorithm,
                        },
                    },
                });
                replTest.startSet();
                replTest.initiate();
                mongod = replTest.getPrimary();

                expectedDynamicWarnings = 0;
                expectedPrioritizationWarnings = 0;
            });

            afterEach(function () {
                if (replTest) {
                    replTest.stopSet();
                }
            });

            it("should allow transitions to other algorithms and enabling/disabling prioritization", function () {
                assertTicketSizing(mongod, {expectDynamicAdjustmentWarnings: true, expectPrioritizationWarnings: true});

                setExecutionControlAlgorithm(mongod, kFixedConcurrentTransactionsAlgorithm);
                assertTicketSizing(mongod, {expectPrioritizationWarnings: true});

                setDeprioritizationGate(mongod, true);
                assertTicketSizing(mongod);

                setExecutionControlAlgorithm(mongod, kThroughputProbingAlgorithm);
                assertTicketSizing(mongod, {expectDynamicAdjustmentWarnings: true});
            });
        });
    });

    describe("Ticket count is preserved across concurrency adjustment algorithm and prioritization transitions", function () {
        let replTest, mongod;

        before(function () {
            replTest = new ReplSetTest({nodes: 1});
            replTest.startSet();
            replTest.initiate();
            mongod = replTest.getPrimary();
        });

        after(function () {
            if (replTest) {
                replTest.stopSet();
            }
        });

        beforeEach(function () {
            setExecutionControlAlgorithm(mongod, kFixedConcurrentTransactionsAlgorithm);
        });

        it("should preserve normal priority ticket values across algorithm changes", function () {
            const customTicketCount = 50;
            setExecutionControlTickets(mongod, {read: customTicketCount});

            setExecutionControlAlgorithm(mongod, kThroughputProbingAlgorithm);
            setExecutionControlAlgorithm(mongod, kFixedConcurrentTransactionsAlgorithm);

            const res = assert.commandWorked(
                mongod.adminCommand({getParameter: 1, executionControlConcurrentReadTransactions: 1}),
            );
            assert.eq(
                customTicketCount,
                res.executionControlConcurrentReadTransactions,
                "Normal priority ticket value was not preserved",
            );
        });

        it("should preserve low priority ticket values across prioritization changes", function () {
            const customTicketCount = 40;
            setDeprioritizationGate(mongod, true);
            setExecutionControlTickets(mongod, {readLowPriority: customTicketCount});

            setDeprioritizationGate(mongod, false);
            setDeprioritizationGate(mongod, true);

            const res = assert.commandWorked(
                mongod.adminCommand({
                    getParameter: 1,
                    executionControlConcurrentReadLowPriorityTransactions: 1,
                }),
            );
            assert.eq(
                customTicketCount,
                res.executionControlConcurrentReadLowPriorityTransactions,
                "Low priority ticket value was not preserved",
            );
        });

        it("should preserve throughput probing parameter values across algorithm changes", function () {
            const customRatio = 0.75;
            assert.commandWorked(mongod.adminCommand({setParameter: 1, throughputProbingReadWriteRatio: customRatio}));

            setExecutionControlAlgorithm(mongod, kThroughputProbingAlgorithm);
            setExecutionControlAlgorithm(mongod, kFixedConcurrentTransactionsAlgorithm);

            const res = assert.commandWorked(
                mongod.adminCommand({getParameter: 1, throughputProbingReadWriteRatio: 1}),
            );
            assert.eq(
                res.throughputProbingReadWriteRatio,
                customRatio,
                "Throughput probing parameter value was not preserved",
            );
        });
    });

    describe("Prioritization transition validation with zero low-priority tickets", function () {
        let replTest, mongod;

        before(function () {
            replTest = new ReplSetTest({nodes: 1});
            replTest.startSet();
            replTest.initiate();
            mongod = replTest.getPrimary();
        });

        after(function () {
            if (replTest) {
                replTest.stopSet();
            }
        });

        beforeEach(function () {
            setDeprioritizationGate(mongod, true);
        });

        afterEach(function () {
            // Reset low-priority tickets to a non-zero value after each test.
            setExecutionControlTickets(mongod, {readLowPriority: 5, writeLowPriority: 5});
        });

        it("should fail to disable prioritization when low-priority read tickets are 0", function () {
            setExecutionControlTickets(mongod, {readLowPriority: 0});

            // Attempt to disable deprioritization should fail.
            assert.commandFailedWithCode(
                mongod.adminCommand({
                    setParameter: 1,
                    executionControlDeprioritizationGate: false,
                }),
                ErrorCodes.IllegalOperation,
            );
        });

        it("should fail to disable prioritization when low-priority write tickets are 0", function () {
            setExecutionControlTickets(mongod, {writeLowPriority: 0});

            // Attempt to disable deprioritization should fail.
            assert.commandFailedWithCode(
                mongod.adminCommand({
                    setParameter: 1,
                    executionControlDeprioritizationGate: false,
                }),
                ErrorCodes.IllegalOperation,
            );
        });

        it("should allow staying in prioritization mode regardless of low-priority ticket values", function () {
            setDeprioritizationGate(mongod, true);

            // Set low-priority tickets to 0.
            setExecutionControlTickets(mongod, {readLowPriority: 0, writeLowPriority: 0});

            // Staying in prioritization mode should succeed.
            setDeprioritizationGate(mongod, true);
        });

        it("should allow enabling deprioritization regardless of low-priority ticket values", function () {
            // Start with non-prioritization mode.
            setDeprioritizationGate(mongod, false);

            // Set low-priority tickets to 0 (this is allowed in non-prioritization modes).
            setExecutionControlTickets(mongod, {readLowPriority: 0, writeLowPriority: 0});

            // Transition to prioritization should succeed even with 0 low-priority tickets.
            setDeprioritizationGate(mongod, true);
        });
    });

    describe("Total tickets alignment after transition from throughput probing", function () {
        let replTest, mongod;

        before(function () {
            replTest = new ReplSetTest({nodes: 1});
            replTest.startSet();
            replTest.initiate();
            mongod = replTest.getPrimary();
        });

        after(function () {
            if (replTest) {
                replTest.stopSet();
            }
        });

        beforeEach(function () {
            setExecutionControlAlgorithm(mongod, kThroughputProbingAlgorithm);
        });

        it("should align tickets when transitioning from throughput probing to fixed concurrency adjustment algorithm", function () {
            const customReadTickets = 30;
            const customWriteTickets = 40;

            // Set custom ticket values while in the fixed concurrency adjustment algorithm.
            setExecutionControlTickets(mongod, {
                read: customReadTickets,
                write: customWriteTickets,
            });

            // Switch to throughput probing. The total tickets may now be different.
            setExecutionControlAlgorithm(mongod, kThroughputProbingAlgorithm);

            // Switch back to fixed concurrency adjustment. This should trigger the alignment.
            setExecutionControlAlgorithm(mongod, kFixedConcurrentTransactionsAlgorithm);

            const stats = getExecutionControlStats(mongod);
            assert.eq(stats.read.totalTickets, customReadTickets);
            assert.eq(stats.write.totalTickets, customWriteTickets);
        });

        it("should align tickets when transitioning from throughput probing to fixed concurrency adjustment algorithm with prioritization", function () {
            setDeprioritizationGate(mongod, true);

            const customReadTickets = 35;
            const customWriteTickets = 45;
            const customLowPrioReadTickets = 15;
            const customLowPrioWriteTickets = 25;

            // Set custom ticket values for all ticket pools.
            setExecutionControlTickets(mongod, {
                read: customReadTickets,
                write: customWriteTickets,
                readLowPriority: customLowPrioReadTickets,
                writeLowPriority: customLowPrioWriteTickets,
            });

            // Switch to throughput probing. The total tickets may now be different.
            setExecutionControlAlgorithm(mongod, kThroughputProbingAlgorithm);

            // Switch back to fixed concurrency adjustment. This should trigger the alignment.
            setExecutionControlAlgorithm(mongod, kFixedConcurrentTransactionsAlgorithm);

            const stats = getExecutionControlStats(mongod);
            assert.eq(stats.read.normalPriority.totalTickets, customReadTickets);
            assert.eq(stats.write.normalPriority.totalTickets, customWriteTickets);
            assert.eq(stats.read.lowPriority.totalTickets, customLowPrioReadTickets);
            assert.eq(stats.write.lowPriority.totalTickets, customLowPrioWriteTickets);
        });

        it("should preserve dynamically adjusted tickets when transitioning from throughput probing to fixed concurrency adjustment algorithm", function () {
            // Capture the current ticket counts that throughput probing may have dynamically
            // adjusted.
            const beforeStats = getExecutionControlStats(mongod);
            const throughputProbingReadTickets = beforeStats.read.normalPriority.totalTickets;
            const throughputProbingWriteTickets = beforeStats.write.normalPriority.totalTickets;

            setExecutionControlTickets(mongod, {
                read: throughputProbingReadTickets,
                write: throughputProbingWriteTickets,
            });

            // Transition to fixed concurrency adjustment algorithm.
            setExecutionControlAlgorithm(mongod, kFixedConcurrentTransactionsAlgorithm);

            // Verify that the previous dynamically determined tickets count are preserved.
            const afterStats = getExecutionControlStats(mongod);
            assert.eq(throughputProbingReadTickets, afterStats.read.normalPriority.totalTickets);
            assert.eq(throughputProbingWriteTickets, afterStats.write.normalPriority.totalTickets);
        });
    });
});
