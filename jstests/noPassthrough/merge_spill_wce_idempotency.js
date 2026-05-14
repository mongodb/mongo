/**
 * Regression test for SERVER-124271:
 *
 *   ContainerBasedSpiller::mergeSpills() drives a MergeIterator from inside
 *   a writeConflictRetry lambda. On WriteConflictException (WCE),
 *   WiredTiger's rollback_transaction() resets every cursor on the session,
 *   including the read cursors backing the MergeIterator's input
 *   ContainerIterators. The merge iterator's in-memory heap, however, is
 *   not rewound, so the next ->next() call either silently skips or
 *   re-yields elements. This produces output spills whose element count
 *   and/or content checksum diverge across WCE retry trials of the same
 *   input.
 *
 *   This test exercises the bug end-to-end by forcing a $group pipeline
 *   to spill across multiple merge passes, then enabling the
 *   WTWriteConflictException failpoint with `times: N` (N >= 2) so that
 *   the writeConflictRetry helper inside mergeSpills() runs the retry
 *   path at least twice on the same outer-pass iterator. The assertions
 *   compare the document count and a deterministic content checksum to a
 *   control run with the failpoint disabled. They MUST be bit-identical.
 *
 *   With the buggy code path the assertion fails (drops/duplicates).
 *   With the proposed fix (drain MergeIterator into a std::vector before
 *   entering the write loop) the assertion passes.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_wiredtiger,
 *   does_not_support_stepdowns,
 *   no_selinux,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {setParameter} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

const conn = MongoRunner.runMongod();
const db = conn.getDB("merge_spill_wce_idempotency");
const collName = jsTestName();
const coll = db[collName];
coll.drop();

// Force spilling for every document and force multiple merge passes by
// capping the per-merge fan-in. This is what makes mergeSpills() actually
// execute the writeConflictRetry path under WCE.
assert.commandWorked(setParameter(db, "internalDocumentSourceCursorInitialBatchSize", 1));
assert.commandWorked(setParameter(db, "internalDocumentSourceCursorBatchSizeBytes", 1));
assert.commandWorked(setParameter(db, "internalDocumentSourceGroupMaxMemoryBytes", 1));
assert.commandWorked(setParameter(db, "internalQuerySlotBasedExecutionHashAggApproxMemoryUseInBytesBeforeSpill", 1));

// Seed deterministic, ordered input so that the merge phase has well-defined
// input keys for both spills. A perfectly sorted input maximises the chance
// the buggy code path picks up an already-consumed (duplicated) head.
const kNumDocs = 200;
const inputDocs = [];
for (let i = 0; i < kNumDocs; ++i) {
    inputDocs.push({_id: i, payload: "x".repeat(64), bucket: i % 8});
}
assert.commandWorked(coll.insertMany(inputDocs));

// Pipeline: $group by bucket, summing _id. The result has exactly 8 rows
// keyed by bucket and a deterministic sum per group. Both the count and
// the per-group sum survive bit-identically across runs *iff* mergeSpills
// is idempotent under WCE retry.
const pipeline = [
    {$group: {_id: "$bucket", total: {$sum: "$_id"}, n: {$sum: 1}}},
    {$sort: {_id: 1}},
];

function checksum(rows) {
    // Order-stable checksum: concatenate (key, total, n) triples after
    // sorting. The pipeline already sorts by _id, but we re-sort
    // defensively so the assertion fails on element-count divergence
    // rather than ordering noise.
    return rows
        .slice()
        .sort((a, b) => a._id - b._id)
        .map((r) => `${r._id}:${r.total}:${r.n}`)
        .join("|");
}

// 1. CONTROL run: failpoint off. Compute the reference result.
const reference = coll.aggregate(pipeline, {allowDiskUse: true}).toArray();
assert.eq(reference.length, 8, `control: expected 8 groups, got ${tojson(reference)}`);
const referenceChecksum = checksum(reference);
const referenceTotal = reference.reduce((acc, r) => acc + r.n, 0);
assert.eq(referenceTotal, kNumDocs, `control: per-group counts must sum to ${kNumDocs}`);

// 2. RETRY run: inject N >= 2 WriteConflictExceptions on the write side
//    of writeConflictRetry. This is the failpoint exercised by
//    ContainerBasedSpiller::mergeSpills_insert (and _remove) per the
//    container_based_spiller_test cases:
//        MergeSpillsRetryOnWriteConflict
//        MergeSpillsTwoWriteConflictsRetryCount
//        MergeSpillsRandomWriteConflicts
//    Using {times: N} with N >= 2 forces back-to-back rollbacks inside
//    the same outer-pass iterator, which is precisely the regime in
//    which the read-cursor reset surfaces as drops or duplicates.
const kNumConflicts = 4;
const fp = configureFailPoint(db, "WTWriteConflictException", {}, {times: kNumConflicts});

let retryResult;
try {
    retryResult = coll.aggregate(pipeline, {allowDiskUse: true}).toArray();
} finally {
    fp.off();
}

// 3. ASSERTIONS: every observable must be bit-identical to the control.
assert.eq(
    retryResult.length,
    reference.length,
    `retry run produced ${retryResult.length} groups, control produced ${reference.length}; ` +
        `retry=${tojson(retryResult)} control=${tojson(reference)}`,
);

const retryChecksum = checksum(retryResult);
assert.eq(
    retryChecksum,
    referenceChecksum,
    `mergeSpills() under WCE retry must be idempotent. ` +
        `retry checksum=${retryChecksum} control checksum=${referenceChecksum}`,
);

const retryTotal = retryResult.reduce((acc, r) => acc + r.n, 0);
assert.eq(
    retryTotal,
    kNumDocs,
    `retry run per-group counts summed to ${retryTotal}; expected ${kNumDocs}. ` +
        `This indicates either (a) the MergeIterator dropped elements when a ` +
        `read cursor was reset by rollback_transaction(), or (b) duplicated ` +
        `elements when the cursor rewound past an already-consumed key.`,
);

// Per-bucket sanity: each input document contributes exactly once to its
// bucket. Group n's must each equal kNumDocs / 8 (= 25 for kNumDocs=200).
const kExpectedPerGroup = kNumDocs / 8;
for (const row of retryResult) {
    assert.eq(
        row.n,
        kExpectedPerGroup,
        `group _id=${row._id} has n=${row.n}; expected ${kExpectedPerGroup}. ` +
            `Per-group count divergence is the smoking gun for the SERVER-124271 ` +
            `MergeIterator cursor-reset bug.`,
    );
}

MongoRunner.stopMongod(conn);
