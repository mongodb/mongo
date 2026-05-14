/**
 * SERVER-126370: Verify that resharding correctly treats applyOps oplog entries stamped with
 * MultiOplogEntryType::kApplyOpsAppliedAtomically as retryable.
 *
 * Vectored inserts whose total size fits within a single storage transaction are written as a
 * single applyOps entry tagged kApplyOpsAppliedAtomically (as opposed to kApplyOpsAppliedSeparately,
 * which is only used when the batch must be split across storage transactions). Prior to this
 * ticket, resharding's session-op writer path only inspected kApplyOpsAppliedSeparately when
 * deciding whether to unroll an applyOps entry into individual retryable ops, which meant atomic
 * applyOps stamped with a retryable session/txnNumber were not surfaced into the recipient's
 * config.transactions history. A retry of the same statement after resharding completed could
 * therefore re-execute the inserts (producing duplicates / DuplicateKeyError) instead of being
 * recognized as a retry.
 *
 * This test exercises a retryable vectored insert whose batch fits in one storage transaction
 * (so it is emitted as a single atomic applyOps) before, during, and after resharding, and asserts
 * that retries are correctly deduplicated at every phase.
 *
 * @tags: [
 *   uses_atclustertime,
 *   requires_fcv_80,
 * ]
 */

import {RetryableWritesUtil} from "jstests/libs/retryable_writes_util.js";
import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";
import {isUweEnabled} from "jstests/libs/query/uwe_utils.js";

function runTest(shouldReshardInPlace) {
    jsTest.log(`Running atomic-applyOps retryable test, reshardInPlace=${shouldReshardInPlace}`);

    const minimumOperationDurationMS = 30000;
    const reshardingTest = new ReshardingTest({
        numDonors: 2,
        numRecipients: 2,
        reshardInPlace: shouldReshardInPlace,
        minimumOperationDurationMS,
    });
    reshardingTest.setup();

    const donorShardNames = reshardingTest.donorShardNames;
    const sourceCollection = reshardingTest.createShardedCollection({
        ns: "reshardingDb.coll",
        shardKeyPattern: {oldKey: 1},
        chunks: [
            {min: {oldKey: MinKey}, max: {oldKey: 0}, shard: donorShardNames[0]},
            {min: {oldKey: 0}, max: {oldKey: MaxKey}, shard: donorShardNames[1]},
        ],
    });

    // Crucially, do NOT lower internalInsertMaxBatchSize. Leaving it at its default ensures each
    // vectored insert below is emitted as ONE applyOps entry tagged kApplyOpsAppliedAtomically
    // rather than split into multiple kApplyOpsAppliedSeparately entries (which is what the
    // existing resharding_retryable_writes.js coverage exercises).
    const mongos = sourceCollection.getMongo();

    // Seed a baseline doc on each donor so update-style retryables have something to touch.
    assert.commandWorked(sourceCollection.insert([
        {_id: "anchor_shard0", oldKey: -10, newKey: -10},
        {_id: "anchor_shard1", oldKey: 10, newKey: 10},
    ]));

    const beforeSession = mongos.startSession({causalConsistency: false, retryWrites: false});
    const duringSession = mongos.startSession({causalConsistency: false, retryWrites: false});
    const beforeSessionColl = beforeSession.getDatabase(sourceCollection.getDB().getName())
                                  .getCollection(sourceCollection.getName());
    const duringSessionColl = duringSession.getDatabase(sourceCollection.getDB().getName())
                                  .getCollection(sourceCollection.getName());

    // A vectored insert whose total wire-payload fits in a single storage transaction. The
    // default internalInsertMaxBatchSize (>= 64) easily covers eight tiny documents, so the
    // op-observer emits exactly one applyOps oplog entry per donor shard, stamped
    // multiOpType: kApplyOpsAppliedAtomically.
    const atomicInsertBefore = {
        insert: sourceCollection.getName(),
        documents: [
            {_id: "atomic_before_0", oldKey: -20, newKey: -20, phase: "before"},
            {_id: "atomic_before_1", oldKey: -20, newKey: 20, phase: "before"},
            {_id: "atomic_before_2", oldKey: -20, newKey: -20, phase: "before"},
            {_id: "atomic_before_3", oldKey: -20, newKey: 20, phase: "before"},
            {_id: "atomic_before_4", oldKey: 20, newKey: 20, phase: "before"},
            {_id: "atomic_before_5", oldKey: 20, newKey: -20, phase: "before"},
            {_id: "atomic_before_6", oldKey: 20, newKey: 20, phase: "before"},
            {_id: "atomic_before_7", oldKey: 20, newKey: -20, phase: "before"},
        ],
        txnNumber: NumberLong(1),
    };

    const atomicInsertDuring = {
        insert: sourceCollection.getName(),
        documents: [
            {_id: "atomic_during_0", oldKey: -20, newKey: -20, phase: "during"},
            {_id: "atomic_during_1", oldKey: -20, newKey: 20, phase: "during"},
            {_id: "atomic_during_2", oldKey: -20, newKey: -20, phase: "during"},
            {_id: "atomic_during_3", oldKey: -20, newKey: 20, phase: "during"},
            {_id: "atomic_during_4", oldKey: 20, newKey: 20, phase: "during"},
            {_id: "atomic_during_5", oldKey: 20, newKey: -20, phase: "during"},
            {_id: "atomic_during_6", oldKey: 20, newKey: 20, phase: "during"},
            {_id: "atomic_during_7", oldKey: 20, newKey: -20, phase: "during"},
        ],
        txnNumber: NumberLong(1),
    };

    const uweEnabled = isUweEnabled(mongos);

    // Helper: re-issue the insert with the same lsid+txnNumber. If retryability is honored,
    // RetryableWritesUtil.runRetryableWrite expects the second invocation to be a no-op. If the
    // atomic applyOps path were dropped on the floor by resharding, the second invocation after
    // resharding would re-execute and surface DuplicateKey on the existing _id values.
    function runAtomicRetryable(sessionColl, command, phase, expectedCode = ErrorCodes.OK) {
        RetryableWritesUtil.runRetryableWrite(sessionColl, command, expectedCode);

        // Every _id in the batch must be visible exactly once; runRetryableWrite() fires the
        // command twice internally, so a missing retryability check would produce duplicates
        // or DuplicateKeyError.
        const tag = command.documents[0].phase;
        const visible = sourceCollection.find({phase: tag}).toArray();
        if (expectedCode === ErrorCodes.OK) {
            assert.eq(command.documents.length, visible.length, {
                message: `atomic-applyOps retryable insert not deduplicated ${phase}`,
                phase: tag,
                visible,
            });
        }
    }

    // Phase 1: before resharding starts. Both invocations land on donors; second invocation must
    // hit the donor's local config.transactions history (already working path; sanity check).
    runAtomicRetryable(beforeSessionColl, atomicInsertBefore, "before resharding");

    const recipientShardNames = reshardingTest.recipientShardNames;
    reshardingTest.withReshardingInBackground(
        {
            newShardKeyPattern: {newKey: 1},
            newChunks: [
                {min: {newKey: MinKey}, max: {newKey: 0}, shard: recipientShardNames[0]},
                {min: {newKey: 0}, max: {newKey: MaxKey}, shard: recipientShardNames[1]},
            ],
        },
        () => {
            // Phase 2: during resharding, before cloneTimestamp. The atomic applyOps will be
            // captured by the donor's oplog and replicated to recipients via the txn cloner.
            assert.soon(() => {
                const doc = mongos.getCollection("config.reshardingOperations").findOne({
                    ns: sourceCollection.getFullName(),
                });
                return doc !== null && doc.cloneTimestamp !== undefined;
            });

            // Phase 3: after cloneTimestamp chosen — this is the critical window where the
            // ReshardingOplogBatchPreparer::makeSessionOpWriterVectors path must recognize the
            // atomic-multiOpType applyOps and surface its session entries to recipients.
            runAtomicRetryable(duringSessionColl, atomicInsertDuring,
                               "during resharding after cloneTimestamp");

            assert.soon(() => {
                const doc = mongos.getCollection("config.reshardingOperations").findOne({
                    ns: sourceCollection.getFullName(),
                });
                return doc !== null && doc.state === "applying";
            });
        },
    );

    // Phase 4: after resharding completes. Retries of statements issued DURING resharding must
    // still be recognized — recipients now own the data, so the retry path consults the
    // recipient-side config.transactions that was populated from the donor's atomic applyOps.
    // Pre-fix behavior: the atomic applyOps was never unrolled into config.transactions on
    // recipients, so this retry either re-executed the inserts (DuplicateKey) or — with UWE
    // enabled — returned IncompleteTransactionHistory inconsistently.
    runAtomicRetryable(duringSessionColl, atomicInsertDuring, "after resharding (retry of during)",
                       ErrorCodes.OK);

    // The before-resharding session's retry path crosses the resharding boundary identically;
    // UWE flips the contract for some retryable paths to IncompleteTransactionHistory, mirroring
    // the existing resharding_retryable_writes.js expectation.
    const beforeExpectedCode = !uweEnabled ? ErrorCodes.OK : ErrorCodes.IncompleteTransactionHistory;
    runAtomicRetryable(beforeSessionColl, atomicInsertBefore, "after resharding (retry of before)",
                       beforeExpectedCode);

    reshardingTest.teardown();
}

runTest(true);
runTest(false);
