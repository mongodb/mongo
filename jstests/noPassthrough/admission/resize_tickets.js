/**
 * Tests that the 'executionControlConcurrencyAdjustmentAlgorithm' can be changed at runtime, and
 * that the server's behavior regarding ticket resizing adjusts accordingly.
 *
 * @tags: [
 *   requires_replication,  # Tickets can only be resized when using the WiredTiger engine.
 *   requires_wiredtiger,
 *   # The test deploys replica sets with a execution control concurrency adjustment configured by
 *   # each test case, which should not be overwritten and expect to have 'throughputProbing' as
 *   # default.
 *   incompatible_with_execution_control_with_prioritization,
 * ]
 */

import {afterEach, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

describe("Execution control concurrency adjustment algorithm", function () {
    const kFixed = "fixedConcurrentTransactions";
    const kFixedWithPrio = "fixedConcurrentTransactionsWithPrioritization";
    const kThroughputProbing = "throughputProbing";

    // Global counters to track expected warning counts across all test calls
    let expectedDynamicWarnings = 0;
    let expectedPrioritizationWarnings = 0;

    /**
     * Sets the concurrency adjustment algorithm at runtime and verifies the change was successful.
     */
    function setAlgorithm(mongod, algorithm) {
        assert.commandWorked(
            mongod.adminCommand({setParameter: 1, executionControlConcurrencyAdjustmentAlgorithm: algorithm}),
        );
    }

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

        const ticketParams = {
            executionControlConcurrentReadTransactions: 25,
            executionControlConcurrentWriteTransactions: 25,
            executionControlConcurrentReadLowPriorityTransactions: 25,
            executionControlConcurrentWriteLowPriorityTransactions: 25,
        };
        for (const [param, value] of Object.entries(ticketParams)) {
            assert.commandWorked(mongod.adminCommand({setParameter: 1, [param]: value}));
        }

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

    describe("Algorithm behavior at server startup", function () {
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

            const algorithm = assert.commandWorked(
                mongod.adminCommand({
                    getParameter: 1,
                    executionControlConcurrencyAdjustmentAlgorithm: 1,
                }),
            ).executionControlConcurrencyAdjustmentAlgorithm;

            if (algorithm === kThroughputProbing) {
                assertTicketSizing(mongod, {expectDynamicAdjustmentWarnings: true, expectPrioritizationWarnings: true});
            }
        });

        it("should allow ticket resizing when using the 'fixed' algorithm", function () {
            replTest = new ReplSetTest({
                nodes: 1,
                nodeOptions: {setParameter: {executionControlConcurrencyAdjustmentAlgorithm: kFixed}},
            });
            replTest.startSet();
            replTest.initiate();
            const mongod = replTest.getPrimary();
            assertTicketSizing(mongod, {expectPrioritizationWarnings: true});
        });

        it("should implicitly use 'fixed' algorithm when tickets are set at startup", function () {
            replTest = new ReplSetTest({
                nodes: 1,
                nodeOptions: {setParameter: {executionControlConcurrentReadTransactions: 20}},
            });
            replTest.startSet();
            replTest.initiate();
            const mongod = replTest.getPrimary();

            const getParameterResult = mongod.adminCommand({
                getParameter: 1,
                executionControlConcurrencyAdjustmentAlgorithm: 1,
            });
            assert.commandWorked(getParameterResult);
            assert.neq(getParameterResult.executionControlConcurrencyAdjustmentAlgorithm, kThroughputProbing);
        });

        it("should not override to 'fixed' algorithm when prioritization is set at startup", function () {
            replTest = new ReplSetTest({
                nodes: 1,
                nodeOptions: {
                    setParameter: {
                        executionControlConcurrencyAdjustmentAlgorithm: kFixedWithPrio,
                        executionControlConcurrentReadTransactions: 20,
                    },
                },
            });
            replTest.startSet();
            replTest.initiate();
            const mongod = replTest.getPrimary();

            const getParameterResult = mongod.adminCommand({
                getParameter: 1,
                executionControlConcurrencyAdjustmentAlgorithm: 1,
            });
            assert.commandWorked(getParameterResult);
            assert.eq(getParameterResult.executionControlConcurrencyAdjustmentAlgorithm, kFixedWithPrio);

            assertTicketSizing(mongod);
        });

        it(`should allow resizing all ticket types with '${kFixedWithPrio}'`, function () {
            replTest = new ReplSetTest({
                nodes: 1,
                nodeOptions: {
                    setParameter: {executionControlConcurrencyAdjustmentAlgorithm: kFixedWithPrio},
                },
            });
            replTest.startSet();
            replTest.initiate();
            const mongod = replTest.getPrimary();
            assertTicketSizing(mongod);
        });
    });

    describe("Algorithm parameter validation", function () {
        let replTest, mongod;

        beforeEach(function () {
            replTest = new ReplSetTest({nodes: 1});
            replTest.startSet();
            replTest.initiate();
            mongod = replTest.getPrimary();

            expectedDynamicWarnings = 0;
            expectedPrioritizationWarnings = 0;
        });

        afterEach(function () {
            replTest.stopSet();
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
            const initialAlgorithm = assert.commandWorked(
                mongod.adminCommand({getParameter: 1, executionControlConcurrencyAdjustmentAlgorithm: 1}),
            ).executionControlConcurrencyAdjustmentAlgorithm;
            setAlgorithm(mongod, initialAlgorithm);
        });
    });

    describe("Runtime transitions", function () {
        describe(`from '${kFixed}'`, function () {
            beforeEach(function () {
                this.replTest = new ReplSetTest({
                    nodes: 1,
                    nodeOptions: {setParameter: {executionControlConcurrencyAdjustmentAlgorithm: kFixed}},
                });
                this.replTest.startSet();
                this.replTest.initiate();
                this.mongod = this.replTest.getPrimary();

                expectedDynamicWarnings = 0;
                expectedPrioritizationWarnings = 0;
            });

            afterEach(function () {
                this.replTest.stopSet();
            });

            it("should allow transitions to other algorithms and back", function () {
                assertTicketSizing(this.mongod, {expectPrioritizationWarnings: true});

                setAlgorithm(this.mongod, kFixedWithPrio);
                assertTicketSizing(this.mongod);

                setAlgorithm(this.mongod, kThroughputProbing);
                assertTicketSizing(this.mongod, {
                    expectDynamicAdjustmentWarnings: true,
                    expectPrioritizationWarnings: true,
                });

                setAlgorithm(this.mongod, kFixed);
                assertTicketSizing(this.mongod, {expectPrioritizationWarnings: true});
            });
        });

        describe(`from '${kFixedWithPrio}'`, function () {
            beforeEach(function () {
                this.replTest = new ReplSetTest({
                    nodes: 1,
                    nodeOptions: {
                        setParameter: {executionControlConcurrencyAdjustmentAlgorithm: kFixedWithPrio},
                    },
                });
                this.replTest.startSet();
                this.replTest.initiate();
                this.mongod = this.replTest.getPrimary();

                expectedDynamicWarnings = 0;
                expectedPrioritizationWarnings = 0;
            });

            afterEach(function () {
                this.replTest.stopSet();
            });

            it("should allow transitions to other algorithms and back", function () {
                assertTicketSizing(this.mongod);

                setAlgorithm(this.mongod, kFixed);
                assertTicketSizing(this.mongod, {expectPrioritizationWarnings: true});

                setAlgorithm(this.mongod, kThroughputProbing);
                assertTicketSizing(this.mongod, {
                    expectDynamicAdjustmentWarnings: true,
                    expectPrioritizationWarnings: true,
                });

                setAlgorithm(this.mongod, kFixedWithPrio);
                assertTicketSizing(this.mongod);
            });
        });

        describe(`from '${kThroughputProbing}'`, function () {
            beforeEach(function () {
                this.replTest = new ReplSetTest({
                    nodes: 1,
                    nodeOptions: {
                        setParameter: {executionControlConcurrencyAdjustmentAlgorithm: kThroughputProbing},
                    },
                });
                this.replTest.startSet();
                this.replTest.initiate();
                this.mongod = this.replTest.getPrimary();

                expectedDynamicWarnings = 0;
                expectedPrioritizationWarnings = 0;
            });

            afterEach(function () {
                this.replTest.stopSet();
            });

            it("should allow transitions to other algorithms and back", function () {
                assertTicketSizing(this.mongod, {
                    expectDynamicAdjustmentWarnings: true,
                    expectPrioritizationWarnings: true,
                });

                setAlgorithm(this.mongod, kFixed);
                assertTicketSizing(this.mongod, {expectPrioritizationWarnings: true});

                setAlgorithm(this.mongod, kFixedWithPrio);
                assertTicketSizing(this.mongod);

                setAlgorithm(this.mongod, kThroughputProbing);
                assertTicketSizing(this.mongod, {
                    expectDynamicAdjustmentWarnings: true,
                    expectPrioritizationWarnings: true,
                });
            });
        });
    });

    describe("Parameter value preservation across transitions", function () {
        let replTest, mongod;

        beforeEach(function () {
            replTest = new ReplSetTest({
                nodes: 1,
                nodeOptions: {setParameter: {executionControlConcurrencyAdjustmentAlgorithm: kFixed}},
            });
            replTest.startSet();
            replTest.initiate();
            mongod = replTest.getPrimary();
        });

        afterEach(function () {
            replTest.stopSet();
        });

        it("should preserve normal priority ticket values across algorithm changes", function () {
            const customTicketCount = 50;
            assert.commandWorked(
                mongod.adminCommand({
                    setParameter: 1,
                    executionControlConcurrentReadTransactions: customTicketCount,
                }),
            );

            setAlgorithm(mongod, kThroughputProbing);
            setAlgorithm(mongod, kFixed);

            const res = assert.commandWorked(
                mongod.adminCommand({getParameter: 1, executionControlConcurrentReadTransactions: 1}),
            );
            assert.eq(
                res.executionControlConcurrentReadTransactions,
                customTicketCount,
                "Normal priority ticket value was not preserved",
            );
        });

        it("should preserve low priority ticket values across algorithm changes", function () {
            const customTicketCount = 40;
            setAlgorithm(mongod, kFixedWithPrio);
            assert.commandWorked(
                mongod.adminCommand({
                    setParameter: 1,
                    executionControlConcurrentReadLowPriorityTransactions: customTicketCount,
                }),
            );

            setAlgorithm(mongod, kFixed);
            setAlgorithm(mongod, kFixedWithPrio);

            const res = assert.commandWorked(
                mongod.adminCommand({getParameter: 1, executionControlConcurrentReadLowPriorityTransactions: 1}),
            );
            assert.eq(
                res.executionControlConcurrentReadLowPriorityTransactions,
                customTicketCount,
                "Low priority ticket value was not preserved",
            );
        });

        it("should preserve throughput probing parameter values across algorithm changes", function () {
            const customRatio = 0.75;
            assert.commandWorked(mongod.adminCommand({setParameter: 1, throughputProbingReadWriteRatio: customRatio}));

            setAlgorithm(mongod, kThroughputProbing);
            setAlgorithm(mongod, kFixed);

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

    describe("Transition validation with zero low-priority tickets", function () {
        let replTest, mongod;

        beforeEach(function () {
            replTest = new ReplSetTest({
                nodes: 1,
                nodeOptions: {
                    setParameter: {executionControlConcurrencyAdjustmentAlgorithm: kFixedWithPrio},
                },
            });
            replTest.startSet();
            replTest.initiate();
            mongod = replTest.getPrimary();
        });

        afterEach(function () {
            assert.commandWorked(
                mongod.adminCommand({
                    setParameter: 1,
                    executionControlConcurrentReadLowPriorityTransactions: 5,
                }),
            );

            assert.commandWorked(
                mongod.adminCommand({
                    setParameter: 1,
                    executionControlConcurrentWriteLowPriorityTransactions: 5,
                }),
            );

            if (replTest) {
                replTest.stopSet();
            }
        });

        it("should fail to transition from prioritization when low-priority read tickets are 0", function () {
            assert.commandWorked(
                mongod.adminCommand({
                    setParameter: 1,
                    executionControlConcurrentReadLowPriorityTransactions: 0,
                }),
            );

            // Attempt to transition to kFixed should fail.
            assert.commandFailedWithCode(
                mongod.adminCommand({setParameter: 1, executionControlConcurrencyAdjustmentAlgorithm: kFixed}),
                ErrorCodes.IllegalOperation,
            );

            // Attempt to transition to kThroughputProbing should also fail.
            assert.commandFailedWithCode(
                mongod.adminCommand({
                    setParameter: 1,
                    executionControlConcurrencyAdjustmentAlgorithm: kThroughputProbing,
                }),
                ErrorCodes.IllegalOperation,
            );
        });

        it("should fail to transition from prioritization when low-priority write tickets are 0", function () {
            assert.commandWorked(
                mongod.adminCommand({
                    setParameter: 1,
                    executionControlConcurrentWriteLowPriorityTransactions: 0,
                }),
            );

            // Attempt to transition to kFixed should fail.
            assert.commandFailedWithCode(
                mongod.adminCommand({setParameter: 1, executionControlConcurrencyAdjustmentAlgorithm: kFixed}),
                ErrorCodes.IllegalOperation,
            );

            // Attempt to transition to kThroughputProbing should also fail.
            assert.commandFailedWithCode(
                mongod.adminCommand({
                    setParameter: 1,
                    executionControlConcurrencyAdjustmentAlgorithm: kThroughputProbing,
                }),
                ErrorCodes.IllegalOperation,
            );
        });

        it("should allow staying in prioritization mode regardless of low-priority ticket values", function () {
            // Set low-priority tickets to 0.
            assert.commandWorked(
                mongod.adminCommand({
                    setParameter: 1,
                    executionControlConcurrentReadLowPriorityTransactions: 0,
                }),
            );
            assert.commandWorked(
                mongod.adminCommand({
                    setParameter: 1,
                    executionControlConcurrentWriteLowPriorityTransactions: 0,
                }),
            );

            // Staying in prioritization mode should succeed.
            setAlgorithm(mongod, kFixedWithPrio);
        });

        it("should allow transitions to prioritization mode regardless of low-priority ticket values", function () {
            // Start with non-prioritization mode.
            setAlgorithm(mongod, kThroughputProbing);

            // Set low-priority tickets to 0 (this is allowed in non-prioritization modes).
            assert.commandWorked(
                mongod.adminCommand({
                    setParameter: 1,
                    executionControlConcurrentReadLowPriorityTransactions: 0,
                }),
            );
            assert.commandWorked(
                mongod.adminCommand({
                    setParameter: 1,
                    executionControlConcurrentWriteLowPriorityTransactions: 0,
                }),
            );

            // Transition to prioritization should succeed even with 0 low-priority tickets.
            setAlgorithm(mongod, kFixedWithPrio);
        });
    });

    describe("Ticket alignment after transition from throughput probing", function () {
        let replTest, mongod;

        afterEach(function () {
            if (replTest) {
                replTest.stopSet();
            }
        });

        it(`should align tickets when transitioning from '${kThroughputProbing}' to '${kFixed}'`, function () {
            replTest = new ReplSetTest({
                nodes: 1,
                nodeOptions: {setParameter: {executionControlConcurrencyAdjustmentAlgorithm: kFixed}},
            });
            replTest.startSet();
            replTest.initiate();
            mongod = replTest.getPrimary();

            const customReadTickets = 30;
            const customWriteTickets = 40;

            // Set custom ticket values while in a 'fixed' mode.
            assert.commandWorked(
                mongod.adminCommand({
                    setParameter: 1,
                    executionControlConcurrentReadTransactions: customReadTickets,
                }),
            );
            assert.commandWorked(
                mongod.adminCommand({
                    setParameter: 1,
                    executionControlConcurrentWriteTransactions: customWriteTickets,
                }),
            );

            // Switch to throughput probing. The total tickets may now be different.
            setAlgorithm(mongod, kThroughputProbing);

            // Switch back to fixed. This should trigger the alignment.
            setAlgorithm(mongod, kFixed);

            const status = assert.commandWorked(mongod.adminCommand({serverStatus: 1}));
            const stats = status.queues.execution;
            assert.eq(stats.read.normalPriority.totalTickets, customReadTickets);
            assert.eq(stats.write.normalPriority.totalTickets, customWriteTickets);
        });

        it(`should align tickets when transitioning from '${kThroughputProbing}' to '${kFixedWithPrio}'`, function () {
            replTest = new ReplSetTest({
                nodes: 1,
                nodeOptions: {
                    setParameter: {executionControlConcurrencyAdjustmentAlgorithm: kFixedWithPrio},
                },
            });
            replTest.startSet();
            replTest.initiate();
            mongod = replTest.getPrimary();

            const customReadTickets = 35;
            const customWriteTickets = 45;
            const customLowPrioReadTickets = 15;
            const customLowPrioWriteTickets = 25;

            // Set custom ticket values for all pools.
            assert.commandWorked(
                mongod.adminCommand({
                    setParameter: 1,
                    executionControlConcurrentReadTransactions: customReadTickets,
                }),
            );
            assert.commandWorked(
                mongod.adminCommand({
                    setParameter: 1,
                    executionControlConcurrentWriteTransactions: customWriteTickets,
                }),
            );
            assert.commandWorked(
                mongod.adminCommand({
                    setParameter: 1,
                    executionControlConcurrentReadLowPriorityTransactions: customLowPrioReadTickets,
                }),
            );
            assert.commandWorked(
                mongod.adminCommand({
                    setParameter: 1,
                    executionControlConcurrentWriteLowPriorityTransactions: customLowPrioWriteTickets,
                }),
            );

            // Switch to throughput probing. The total tickets may now be different.
            setAlgorithm(mongod, kThroughputProbing);

            // Switch back to fixed with prioritization. This should trigger the alignment.
            setAlgorithm(mongod, kFixedWithPrio);

            const status = assert.commandWorked(mongod.adminCommand({serverStatus: 1}));
            const stats = status.queues.execution;
            assert.eq(stats.read.normalPriority.totalTickets, customReadTickets);
            assert.eq(stats.write.normalPriority.totalTickets, customWriteTickets);
            assert.eq(stats.read.lowPriority.totalTickets, customLowPrioReadTickets);
            assert.eq(stats.write.lowPriority.totalTickets, customLowPrioWriteTickets);
        });

        it(`should preserve dynamically adjusted tickets when transitioning from '${
            kThroughputProbing
        }' to '${kFixedWithPrio}'`, function () {
            replTest = new ReplSetTest({
                nodes: 1,
                nodeOptions: {
                    setParameter: {
                        executionControlConcurrencyAdjustmentAlgorithm: kThroughputProbing,
                    },
                },
            });
            replTest.startSet();
            replTest.initiate();
            mongod = replTest.getPrimary();

            // Capture the current ticket counts that throughput probing may have dynamically
            // adjusted.
            const beforeStatus = assert.commandWorked(mongod.adminCommand({serverStatus: 1}));
            const beforeStats = beforeStatus.queues.execution;
            const throughputProbingReadTickets = beforeStats.read.normalPriority.totalTickets;
            const throughputProbingWriteTickets = beforeStats.write.normalPriority.totalTickets;

            assert.commandWorked(
                mongod.adminCommand({
                    setParameter: 1,
                    executionControlConcurrentReadTransactions: throughputProbingReadTickets,
                }),
            );
            assert.commandWorked(
                mongod.adminCommand({
                    setParameter: 1,
                    executionControlConcurrentWriteTransactions: throughputProbingWriteTickets,
                }),
            );

            // Transition to prioritization algorithm.
            setAlgorithm(mongod, kFixedWithPrio);

            // Verify that the previous dynamically determined tickets count are preserved.
            const afterStatus = assert.commandWorked(mongod.adminCommand({serverStatus: 1}));
            const afterStats = afterStatus.queues.execution;
            assert.eq(throughputProbingReadTickets, afterStats.read.normalPriority.totalTickets);
            assert.eq(throughputProbingWriteTickets, afterStats.write.normalPriority.totalTickets);
        });
    });
});
