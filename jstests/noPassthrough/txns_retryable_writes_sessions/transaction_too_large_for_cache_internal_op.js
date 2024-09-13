/**
 * Validate that internal operations a convert TransactionTooLargeForCacheException to a
 * WriteConflictException and handle it accordingly. This test uses a secondary replication worker
 * as the internal operation.
 *
 * @tags: [
 *   # Exclude in-memory engine, rollbacks due to pinned cache content rely on eviction.
 *   requires_persistence,
 *   requires_replication,
 *   requires_wiredtiger,
 *   # TODO(SERVER-90387): remove this JS test after unit test is available.
 *   __TEMPORARILY_DISABLED__,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

const replSet = new ReplSetTest({
    nodes: [
        {},
        {
            // Pin node[1] as the secondary.
            rsConfig: {priority: 0},
        },
    ],
});

replSet.startSet();
replSet.initiate();
const db = replSet.getPrimary().getDB("test");
const secondaryDb = replSet.getSecondary().getDB("test");

// Shrink the WiredTiger cache and lower the TransactionTooLargeForCache threshold to the floor so
// as to reliably get a TransactionTooLargeForCacheException on the secondary.
assert.commandWorked(
    secondaryDb.adminCommand({setParameter: 1, "wiredTigerEngineRuntimeConfig": "cache_size=32M"}));
assert.commandWorked(
    secondaryDb.adminCommand({setParameter: 1, transactionTooLargeForCacheThreshold: 0}));

// Generate an insert operation that pins enough dirty data in the WiredTiger cache on the secondary
// to roll back on replication. This workload is adapted from the reproducer in the SERVER-61909
// ticket description.
assert.commandWorked(db.c.createIndex({x: "text"}));
let doc = {x: []};
for (let j = 0; j < 50000; j++)
    doc.x.push("" + Math.random() + Math.random());

const secondaryOpMetricsBefore = secondaryDb.serverStatus().metrics.operation;

assert.commandWorked(db.c.insert(doc, {writeConcern: {w: 1}}));

const transactionTooLargeForCacheThreshold = 5;
let secondaryOpMetricsAfter;
assert.soon(
    () => {
        secondaryOpMetricsAfter = secondaryDb.serverStatus().metrics.operation;
        return secondaryOpMetricsAfter.transactionTooLargeForCacheErrorsConvertedToWriteConflict >=
            secondaryOpMetricsBefore.transactionTooLargeForCacheErrorsConvertedToWriteConflict +
            transactionTooLargeForCacheThreshold;
    },
    "unable to trigger a TransactionTooLargeForCacheException -> WriteConflictException on the secondary",
    5 * 60 * 1000);

assert.gte(secondaryOpMetricsAfter.transactionTooLargeForCacheErrors,
           secondaryOpMetricsBefore.transactionTooLargeForCacheErrors +
               transactionTooLargeForCacheThreshold);

// Restore the original WiredTiger cache size to allow the secondary to complete replication and for
// the test to succeed.
assert.commandWorked(
    secondaryDb.adminCommand({setParameter: 1, "wiredTigerEngineRuntimeConfig": "cache_size=1G"}));

replSet.stopSet();
