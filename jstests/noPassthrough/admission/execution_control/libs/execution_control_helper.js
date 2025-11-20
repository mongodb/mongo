/**
 * Helper functions for execution control tests.
 *
 * This module provides utilities for testing execution control features including:
 * - Concurrency adjustment algorithms (fixed vs throughput probing)
 * - Operation prioritization (heuristic and background task deprioritization)
 * - Ticket management and statistics
 */

/**
 * Concurrency adjustment algorithm names.
 */
export const kFixedConcurrentTransactionsAlgorithm = "fixedConcurrentTransactions";
export const kFixedConcurrentTransactionsWithPrioritizationAlgorithm = "fixedConcurrentTransactionsWithPrioritization";
export const kThroughputProbingAlgorithm = "throughputProbing";

/**
 * Sets an execution control server parameter on the specified node.
 */
function setExecutionControlParameter(node, parameterName, value) {
    assert.commandWorked(node.adminCommand({setParameter: 1, [parameterName]: value}));
}

/**
 * Gets an execution control server parameter value from the specified node.
 */
function getExecutionControlParameter(node, parameterName) {
    const result = assert.commandWorked(node.adminCommand({getParameter: 1, [parameterName]: 1}));
    return result[parameterName];
}

/**
 * Sets the execution control concurrency adjustment algorithm.
 */
export function setExecutionControlAlgorithm(node, algorithm) {
    setExecutionControlParameter(node, "executionControlConcurrencyAdjustmentAlgorithm", algorithm);
}

/**
 * Gets the current execution control concurrency adjustment algorithm.
 */
export function getExecutionControlAlgorithm(node) {
    return getExecutionControlParameter(node, "executionControlConcurrencyAdjustmentAlgorithm");
}

/**
 * Enables or disables heuristic deprioritization for long-running operations.
 */
export function setHeuristicDeprioritization(node, enabled) {
    setExecutionControlParameter(node, "executionControlHeuristicDeprioritization", enabled);
}

/**
 * Enables or disables background task deprioritization (for index builds, TTL, range deletions).
 */
export function setBackgroundTaskDeprioritization(node, enabled) {
    setExecutionControlParameter(node, "executionControlBackgroundTasksDeprioritization", enabled);
}

/**
 * Retrieves the execution control statistics from serverStatus.
 */
export function getExecutionControlStats(node) {
    return node.getDB("admin").serverStatus().queues.execution;
}

/**
 * Gets the count of finished low-priority read operations.
 */
export function getLowPriorityReadCount(node) {
    return node.adminCommand({serverStatus: 1}).queues.execution.read.lowPriority.finishedProcessing;
}

/**
 * Gets the count of finished low-priority write operations.
 */
export function getLowPriorityWriteCount(node) {
    return node.adminCommand({serverStatus: 1}).queues.execution.write.lowPriority.finishedProcessing;
}

/**
 * Gets the total number of operations that have been deprioritized.
 */
export function getTotalDeprioritizationCount(node) {
    return node.adminCommand({serverStatus: 1}).queues.execution.totalDeprioritizations;
}

/**
 * Sets execution control ticket limits for normal and low priority pools.
 */
export function setExecutionControlTickets(node, limits = {}) {
    const paramMap = {
        read: "executionControlConcurrentReadTransactions",
        write: "executionControlConcurrentWriteTransactions",
        readLowPriority: "executionControlConcurrentReadLowPriorityTransactions",
        writeLowPriority: "executionControlConcurrentWriteLowPriorityTransactions",
    };

    for (const [key, paramName] of Object.entries(paramMap)) {
        if (limits[key] === undefined || limits[key] === null) {
            continue;
        }
        assert.commandWorked(node.adminCommand({setParameter: 1, [paramName]: limits[key]}));
    }
}

/**
 * Generates a random lowercase alphabetic string of specified length.
 */
export function generateRandomString(length) {
    const chars = "abcdefghijklmnopqrstuvwxyz";
    let result = "";
    for (let i = 0; i < length; i++) {
        result += chars.charAt(Math.floor(Math.random() * chars.length));
    }
    return result;
}

/**
 * Inserts test documents into a collection with configurable options.
 */
export function insertTestDocuments(coll, documentCount, options = {}) {
    const {
        startId = 0,
        payloadSize = 256,
        includeRandomString = false,
        randomStringLength = 100,
        extraFields = {},
        docGenerator = null,
    } = options;

    const bulk = coll.initializeUnorderedBulkOp();
    const payload = "x".repeat(payloadSize);

    for (let i = startId; i < startId + documentCount; i++) {
        if (docGenerator) {
            // Use custom document generator if provided
            bulk.insert(docGenerator(i, payload));
        } else {
            // Use default document structure
            const doc = {_id: i, payload: payload, ...extraFields};
            if (includeRandomString) {
                doc.randomStr = generateRandomString(randomStringLength);
            }
            bulk.insert(doc);
        }
    }

    return assert.commandWorked(bulk.execute());
}
