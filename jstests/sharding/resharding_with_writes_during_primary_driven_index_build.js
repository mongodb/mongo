/**
 * Tests that resharding correctly handles retryable writes wrapped in atomically-applied applyOps
 * entries (the atomic-write group type). A primary-driven index build is used to produce these
 * entries: while the build is paused, any retryable write on the collection is packed into an
 * applyOps with multiOpType=kApplyOpsAppliedAtomically.
 *
 * Two code paths for transferring session records to resharding recipients are exercised:
 *   1. Collection cloning — writes issued before resharding starts. The session records are
 *      bulk-copied from the donor's config.transactions to the recipient.
 *   2. Oplog application — writes issued after the cloneTimestamp is chosen. The session records
 *      arrive at the recipient by applying the donor's oplog entries, which include the
 *      atomically-applied applyOps entries.
 *
 * @tags: [
 *   uses_atclustertime,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {RetryableWritesUtil} from "jstests/libs/retryable_writes_util.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";
import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";

// A retryable write replayed after resharding must never re-apply -- that is the durable
// guarantee, verified by the collection state at each call site. The *return code*, however, is
// not stable for a scatter-gather (by-_id) retry: ReshardingTxnCloner clones the donor's entire
// config.transactions to every recipient and installs a dead-end incomplete-history sentinel for
// each session not already executed on that recipient. So a broadcast retry can surface
// IncompleteTransactionHistory from a non-owning shard even while the owning shard returns the
// cached result (yielding a top-level ok:1 with an IncompleteTransactionHistory writeError).
// Accept either a clean success or an IncompleteTransactionHistory-only failure; the no-double-
// apply invariant is asserted separately via the collection contents.
function assertRetryDoesNotReapply(coll, command) {
    let res;
    for (let i = 0; i < 2; i++) {
        res = coll.runCommand(command);
    }
    const writeErrors = res.writeErrors ?? [];
    const onlyIncompleteHistory = writeErrors.every(
        (e) => e.code === ErrorCodes.IncompleteTransactionHistory,
    );
    const topLevelOk = res.ok === 1 || res.code === ErrorCodes.IncompleteTransactionHistory;
    assert(
        topLevelOk && onlyIncompleteHistory,
        "retry after resharding must not re-apply: expected success or IncompleteTransactionHistory only",
        {res},
    );
    return res;
}

describe("resharding with retryable writes in atomically-applied applyOps entries", function () {
    before(function () {
        this.reshardingTest = new ReshardingTest({
            numDonors: 2,
            numRecipients: 2,
            reshardInPlace: true,
        });
        this.reshardingTest.setup();

        const donorShardNames = this.reshardingTest.donorShardNames;
        this.sourceCollection = this.reshardingTest.createShardedCollection({
            ns: "reshardingDb.coll",
            shardKeyPattern: {oldKey: 1},
            chunks: [
                {min: {oldKey: MinKey}, max: {oldKey: 0}, shard: donorShardNames[0]},
                {min: {oldKey: 0}, max: {oldKey: MaxKey}, shard: donorShardNames[1]},
            ],
        });

        const mongos = this.sourceCollection.getMongo();
        this.mongos = mongos;
        const dbName = this.sourceCollection.getDB().getName();
        const collName = this.sourceCollection.getName();

        const shard0Rst = this.reshardingTest.getReplSetForShard(donorShardNames[0]);
        this.shard0Primary = shard0Rst.getPrimary();

        if (
            !FeatureFlagUtil.isPresentAndEnabled(
                this.shard0Primary.getDB(dbName),
                "PrimaryDrivenIndexBuilds",
            )
        ) {
            jsTest.log.info(
                "Skipping test because featureFlagPrimaryDrivenIndexBuilds is disabled",
            );
            this.reshardingTest.teardown();
            quit();
        }

        if (
            !FeatureFlagUtil.isPresentAndEnabled(
                this.shard0Primary.getDB(dbName),
                "ContainerWrites",
            )
        ) {
            jsTest.log.info("Skipping test because featureFlagContainerWrites is disabled");
            this.reshardingTest.teardown();
            quit();
        }

        assert.commandWorked(
            this.sourceCollection.insert([
                {_id: "on_shard0", oldKey: -10, newKey: -10, counter: 0},
                {_id: "on_shard1", oldKey: 10, newKey: 10, counter: 0},
            ]),
        );

        // Pause the index build on shard0 so that subsequent retryable writes get packed into
        // atomically-applied applyOps entries.
        const shard0DB = this.shard0Primary.getDB(dbName);
        IndexBuildTest.pauseIndexBuilds(this.shard0Primary);
        const awaitIndex = IndexBuildTest.startIndexBuild(
            this.shard0Primary,
            this.sourceCollection.getFullName(),
            {oldKey: 1, counter: 1},
            {name: "oldKey_1_counter_1"},
        );
        IndexBuildTest.waitForIndexBuildToStart(shard0DB, collName, "oldKey_1_counter_1");

        // Use separate sessions for update and insert so each can be retried independently
        // without hitting TransactionTooOld from advancing the other's txnNumber.
        const updateSession = mongos.startSession({causalConsistency: false, retryWrites: false});
        this.updateSessionColl = updateSession.getDatabase(dbName).getCollection(collName);

        const insertSession = mongos.startSession({causalConsistency: false, retryWrites: false});
        this.insertSessionColl = insertSession.getDatabase(dbName).getCollection(collName);

        this.updateCommand = {
            update: collName,
            updates: [{q: {_id: "on_shard0"}, u: {$inc: {counter: 1}}}],
            txnNumber: NumberLong(1),
        };

        this.insertCommand = {
            insert: collName,
            documents: [{_id: "inserted_during_pdib", oldKey: -20, newKey: -20, tag: "before"}],
            txnNumber: NumberLong(1),
        };

        jsTest.log.info("Issuing retryable update while index build is paused");
        assert.commandWorked(this.updateSessionColl.runCommand(this.updateCommand));

        jsTest.log.info("Issuing retryable insert while index build is paused");
        assert.commandWorked(this.insertSessionColl.runCommand(this.insertCommand));

        IndexBuildTest.resumeIndexBuilds(this.shard0Primary);
        awaitIndex();
    });

    after(function () {
        this.reshardingTest.teardown();
    });

    it("retries work before resharding", function () {
        RetryableWritesUtil.runRetryableWrite(this.updateSessionColl, this.updateCommand);
        assert.eq(
            1,
            this.sourceCollection.findOne({_id: "on_shard0"}).counter,
            "counter should remain 1 after retry",
        );
        RetryableWritesUtil.runRetryableWrite(this.insertSessionColl, this.insertCommand);
        assert.eq(
            1,
            this.sourceCollection.countDocuments({_id: "inserted_during_pdib"}),
            "insert should not be duplicated by retry",
        );
    });

    it("session records cloned to recipients preserve retryability", function () {
        const recipientShardNames = this.reshardingTest.recipientShardNames;
        this.reshardingTest.withReshardingInBackground(
            {
                newShardKeyPattern: {newKey: 1},
                newChunks: [
                    {min: {newKey: MinKey}, max: {newKey: 0}, shard: recipientShardNames[0]},
                    {min: {newKey: 0}, max: {newKey: MaxKey}, shard: recipientShardNames[1]},
                ],
            },
            () => {
                assert.soon(() => {
                    const coordinatorDoc = this.mongos
                        .getCollection("config.reshardingOperations")
                        .findOne({
                            ns: this.sourceCollection.getFullName(),
                        });
                    return coordinatorDoc !== null && coordinatorDoc.cloneTimestamp !== undefined;
                }, "timed out waiting for resharding coordinator cloneTimestamp");

                jsTest.log.info("Retrying writes during resharding");
                RetryableWritesUtil.runRetryableWrite(this.updateSessionColl, this.updateCommand);
                assert.eq(
                    1,
                    this.sourceCollection.findOne({_id: "on_shard0"}).counter,
                    "counter should remain 1 during resharding retry",
                );

                RetryableWritesUtil.runRetryableWrite(this.insertSessionColl, this.insertCommand);
                assert.eq(
                    1,
                    this.sourceCollection.countDocuments({_id: "inserted_during_pdib"}),
                    "insert should not be duplicated by retry during resharding",
                );
            },
        );

        jsTest.log.info("Retrying writes after resharding");
        assertRetryDoesNotReapply(this.updateSessionColl, this.updateCommand);
        assert.eq(
            1,
            this.sourceCollection.findOne({_id: "on_shard0"}).counter,
            "counter should remain 1 after resharding retry",
        );

        assertRetryDoesNotReapply(this.insertSessionColl, this.insertCommand);
        assert.eq(
            1,
            this.sourceCollection.countDocuments({_id: "inserted_during_pdib"}),
            "insert should not be duplicated by retry after resharding",
        );
    });

    it("session records applied via oplog preserve retryability", function () {
        const dbName = this.sourceCollection.getDB().getName();
        const collName = this.sourceCollection.getName();
        const shard0DB = this.shard0Primary.getDB(dbName);
        const recipientShardNames = this.reshardingTest.recipientShardNames;

        const duringSession = this.mongos.startSession({
            causalConsistency: false,
            retryWrites: false,
        });
        const duringSessionColl = duringSession.getDatabase(dbName).getCollection(collName);
        const duringInsertCommand = {
            insert: collName,
            documents: [{_id: "during_resharding_pdib", oldKey: -30, newKey: -30, tag: "during"}],
            txnNumber: NumberLong(1),
        };

        // The retryable write must land after the cloneTimestamp (so its session record reaches
        // the recipient via oplog application rather than cloning) while a primary-driven index
        // build is in progress (so it is packed into an atomically-applied applyOps entry).
        // createIndexes is rejected once a reshard is in progress (ReshardCollectionInProgress),
        // so the index build is started and paused BEFORE resharding and held across it.
        const setupFP = configureFailPoint(this.shard0Primary, "hangAfterSettingUpIndexBuild");
        const awaitIndex = IndexBuildTest.startIndexBuild(
            this.shard0Primary,
            this.sourceCollection.getFullName(),
            {tag: 1},
            {name: "tag_1"},
        );
        setupFP.wait();

        const listRes = assert.commandWorked(
            shard0DB.runCommand({listIndexes: collName, includeIndexBuildInfo: true}),
        );
        const tagEntry = listRes.cursor.firstBatch.find((e) => e.spec.name === "tag_1");
        const buildUUID = extractUUIDFromObject(tagEntry.indexBuildInfo.buildUUID);

        // The already-paused build stays paused; only newly starting builds re-evaluate the
        // filter and are therefore not held.
        configureFailPoint(this.shard0Primary, "hangAfterSettingUpIndexBuild", {
            buildUUIDs: [buildUUID],
        });

        this.reshardingTest.withReshardingInBackground(
            {
                newShardKeyPattern: {oldKey: 1},
                newChunks: [
                    {min: {oldKey: MinKey}, max: {oldKey: 0}, shard: recipientShardNames[0]},
                    {min: {oldKey: 0}, max: {oldKey: MaxKey}, shard: recipientShardNames[1]},
                ],
            },
            () => {
                assert.soon(() => {
                    const coordinatorDoc = this.mongos
                        .getCollection("config.reshardingOperations")
                        .findOne({
                            ns: this.sourceCollection.getFullName(),
                        });
                    return coordinatorDoc !== null && coordinatorDoc.cloneTimestamp !== undefined;
                }, "timed out waiting for resharding coordinator cloneTimestamp");

                jsTest.log.info(
                    "Issuing retryable insert during resharding while index build is paused",
                );
                assert.commandWorked(duringSessionColl.runCommand(duringInsertCommand));

                // Resume the build so it -- and the reshard -- can complete.
                setupFP.off();
                awaitIndex();

                RetryableWritesUtil.runRetryableWrite(duringSessionColl, duringInsertCommand);
                assert.eq(
                    1,
                    this.sourceCollection.countDocuments({_id: "during_resharding_pdib"}),
                    "during-resharding insert should not be duplicated",
                );
            },
        );

        // Resharding preserves retryability for writes made during the operation.
        jsTest.log.info("Retrying during-resharding write after resharding");
        assertRetryDoesNotReapply(duringSessionColl, duringInsertCommand);
        assert.eq(
            1,
            this.sourceCollection.countDocuments({_id: "during_resharding_pdib"}),
            "during-resharding insert should still be retryable after resharding",
        );
    });
});
