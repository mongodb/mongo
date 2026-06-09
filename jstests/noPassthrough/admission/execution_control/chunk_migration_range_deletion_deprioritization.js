/**
 * Tests that chunk migration transfer mods and session oplog fetches are marked as
 * non-deprioritizable during the critical section. Verifies that the
 * totalMarkedNonDeprioritizable counter in serverStatus increases on both donor and
 * recipient during a migration that has transfer mods in the critical section.
 *
 * Later, verifies that range deleter operations are executed as deprioritizable.
 * @tags: [
 *   requires_sharding,
 *   requires_fcv_83,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    getTotalDeprioritizationCount,
    getTotalMarkedNonDeprioritizableCount,
} from "jstests/noPassthrough/admission/execution_control/libs/execution_control_helper.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";

const dbName = jsTestName();
const collName = "coll";
const ns = dbName + "." + collName;

const st = new ShardingTest({
    shards: 2,
    other: {
        rsOptions: {
            setParameter: {
                executionControlDeprioritizationGate: true,
                disableResumableRangeDeleter: true,
            },
        },
    },
});

const mongos = st.s;
const testDB = mongos.getDB(dbName);
const testColl = testDB.getCollection(collName);

assert.commandWorked(mongos.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {x: 1}}));

const numInitialDocs = 100;
const bulk = testColl.initializeUnorderedBulkOp();
for (let i = 0; i < numInitialDocs; i++) {
    bulk.insert({_id: i, x: i, payload: "y".repeat(256)});
}
assert.commandWorked(bulk.execute());

const donorPrimary = st.rs0.getPrimary();
const recipientPrimary = st.rs1.getPrimary();

const hangBeforeCriticalSectionFp = configureFailPoint(donorPrimary, "hangBeforeEnteringCriticalSection");

jsTest.log.info("Starting moveChunk in background");
const awaitResult = startParallelShell(
    funWithArgs(
        function (ns, toShardName) {
            assert.commandWorked(db.adminCommand({moveChunk: ns, find: {x: 0}, to: toShardName}));
        },
        ns,
        st.shard1.shardName,
    ),
    mongos.port,
);

jsTest.log.info("Waiting for migration to pause before critical section");
hangBeforeCriticalSectionFp.wait();

jsTest.log.info("Inserting documents while migration is paused before critical section");
const numExtraDocs = 20;
for (let i = numInitialDocs; i < numInitialDocs + numExtraDocs; i++) {
    assert.commandWorked(testColl.insert({_id: i, x: i, payload: "z".repeat(256)}));
}
for (let i = 0; i < 10; i++) {
    assert.commandWorked(testColl.update({_id: i}, {$set: {payload: "updated"}}));
}

const donorCountNonDeprioritizableBeforeMigration = getTotalMarkedNonDeprioritizableCount(donorPrimary);
const recipientCountNonDeprioritizableBeforeMigration = getTotalMarkedNonDeprioritizableCount(recipientPrimary);

jsTest.log.info(
    "Donor totalMarkedNonDeprioritizable before critical section: " + donorCountNonDeprioritizableBeforeMigration,
);
jsTest.log.info(
    "Recipient totalMarkedNonDeprioritizable before critical section: " +
        recipientCountNonDeprioritizableBeforeMigration,
);

jsTest.log.info("Resuming migration into critical section");
hangBeforeCriticalSectionFp.off();

awaitResult();

const donorCountNonDeprioritizableAfterMigration = getTotalMarkedNonDeprioritizableCount(donorPrimary);
const recipientCountNonDeprioritizableAfterMigration = getTotalMarkedNonDeprioritizableCount(recipientPrimary);

jsTest.log.info("Donor totalMarkedNonDeprioritizable after migration: " + donorCountNonDeprioritizableAfterMigration);
jsTest.log.info(
    "Recipient totalMarkedNonDeprioritizable after migration: " + recipientCountNonDeprioritizableAfterMigration,
);

assert.gt(
    donorCountNonDeprioritizableAfterMigration,
    donorCountNonDeprioritizableBeforeMigration,
    "Donor should have marked operations as non-deprioritizable during critical section",
);
assert.gt(
    recipientCountNonDeprioritizableAfterMigration,
    recipientCountNonDeprioritizableBeforeMigration,
    "Recipient should have marked operations as non-deprioritizable during critical section",
);

const donorCountDeprioritizableBeforeRangeDeletion = getTotalDeprioritizationCount(donorPrimary);

assert.commandWorked(donorPrimary.adminCommand({setParameter: 1, disableResumableRangeDeleter: false}));
assert.soon(() => {
    return donorPrimary.getCollection("config.rangeDeletions").countDocuments({}) === 0;
}, "Timed out waiting for range deletions to complete on donor");

const donorCountDeprioritizableAfterRangeDeletion = getTotalDeprioritizationCount(donorPrimary);
assert.gt(
    donorCountDeprioritizableAfterRangeDeletion,
    donorCountDeprioritizableBeforeRangeDeletion,
    "Donor should have executed range deletions as deprioritizable",
);

st.stop();
