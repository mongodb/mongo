/**
 * Tests that the TTL deleter accesses the storage engine with low priority when enabled,
 * and with normal priority when storageEngineDeprioritizeBackgroundTasks is disabled.
 *
 * @tags: [
 *      requires_wiredtiger,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({
    nodes: 1,
    nodeOptions: {
        setParameter: {
            ttlMonitorSleepSecs: 1,
            ttlMonitorEnabled: false,
            storageEngineConcurrencyAdjustmentAlgorithm: "fixedConcurrentTransactionsWithPrioritization",
            storageEngineHeuristicDeprioritizationEnabled: false,
        },
    },
});

rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const dbName = jsTestName();
const coll = primary.getDB(dbName).coll;

// Create a TTL index to expire documents after 0 seconds.
assert.commandWorked(coll.createIndex({expireAt: 1}, {expireAfterSeconds: 0}));

/**
 * Helper function to get the number of finished low-priority write operations on the primary.
 */
const numLowPriorityWrites = function () {
    const status = primary.adminCommand({serverStatus: 1});
    return status.queues.execution.write.lowPriority.finishedProcessing;
};

/**
 * Inserts a specified number of expired documents into the collection.
 */
const insertExpiredDocs = function (coll, {numDocs, startId = 0, payloadSize = 1024}) {
    jsTest.log.info(`Inserting ${numDocs} expired documents starting from _id ${startId}...`);

    const bulk = coll.initializeUnorderedBulkOp();
    const pastDate = new Date(Date.now() - 5000); // A time in the recent past.
    const endId = startId + numDocs;

    for (let i = startId; i < endId; i++) {
        // Add a payload to make the documents non-trivial in size.
        bulk.insert({_id: i, expireAt: pastDate, payload: "x".repeat(payloadSize)});
    }
    assert.commandWorked(bulk.execute());
    assert.eq(numDocs, coll.countDocuments({}), "Count after insertion does not match expected number of docs");
};

/**
 * Waits for the TTL monitor to clear all documents from the collection.
 */
const waitForTTLDeletion = function (coll) {
    assert.soon(
        () => coll.countDocuments({}) === 0,
        "TTL monitor did not delete all documents within the timeout",
        10 * 1000, // 10 seconds
    );
};

jsTest.log.info("Check for low-priority writes when deprioritization is enabled");
{
    insertExpiredDocs(coll, {numDocs: 2000, startId: 0, payloadSize: 1024});

    const lowPriorityBefore = numLowPriorityWrites();

    // Enable the TTL monitor to start the deletions.
    assert.commandWorked(primary.adminCommand({setParameter: 1, ttlMonitorEnabled: true}));

    waitForTTLDeletion(coll);

    const lowPriorityAfter = numLowPriorityWrites();

    // Measure the counter again and assert that it has increased.
    assert.gt(
        lowPriorityAfter,
        lowPriorityBefore,
        "The number of low-priority writes should increase after a TTL deletion runs",
    );

    // Disable the TTL monitor to stop further deletions.
    assert.commandWorked(primary.adminCommand({setParameter: 1, ttlMonitorEnabled: false}));
}

jsTest.log.info("Check for normal-priority writes when deprioritization is disabled");
{
    assert.commandWorked(primary.adminCommand({setParameter: 1, storageEngineDeprioritizeBackgroundTasks: false}));

    // Insert a new batch of documents to be deleted.
    insertExpiredDocs(coll, {numDocs: 1000, startId: 2000, payloadSize: 512});

    const lowPriorityBefore = numLowPriorityWrites();

    // Enable the TTL monitor to start the deletions.
    assert.commandWorked(primary.adminCommand({setParameter: 1, ttlMonitorEnabled: true}));

    waitForTTLDeletion(coll);

    const lowPriorityAfter = numLowPriorityWrites();

    // The counter should NOT have increased this time.
    assert.eq(
        lowPriorityAfter,
        lowPriorityBefore,
        "Should NOT see low-priority writes when storageEngineDeprioritizeBackgroundTasks is disabled",
    );
}

rst.stopSet();
