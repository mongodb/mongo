/**
 * Resume safety for the batched updateLookup stage (BatchedEnrichmentStage).
 *
 * The stage buffers ("fills") several change events ahead of emission, so at a getMore boundary it
 * can hold events that were scanned upstream but not yet returned to the client. This test forces a
 * batch size > 1 and asserts that the postBatchResumeToken reported at such a boundary points at the
 * last *emitted* event, never at a buffered-but-unreturned one: resuming from that token replays
 * exactly the unreturned events with no loss or duplication.
 *
 * Batching is active only with featureFlagChangeStreamOptimizedUpdateLookup on (the all-feature-flags
 * variant); with the flag off the stage runs batch-of-one and the test still passes trivially.
 *
 * @tags: [
 *   assumes_stable_shard_list,
 *   requires_majority_read_concern,
 *   requires_fcv_90,
 * ]
 */
import {
    assertDropAndRecreateCollection,
    assertDropCollection,
} from "jstests/libs/collection_drop_recreate.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {describe, it, beforeEach, afterEach} from "jstests/libs/mochalite.js";
import {getClusterTime, withChangeStreamTest} from "jstests/libs/query/change_stream_util.js";
import {runWithParamsAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

describe("batched updateLookup resume safety", function () {
    const kMaxBatchSize = 100;
    const kNumUpdates = 6;
    const collName = "update_lookup_batching_resume";
    let coll;

    // The expected shape of the change event produced by "$set: {c: c}". updateLookup re-reads
    // the document's *current* state at getMore time rather than a point-in-time snapshot, and
    // every update below runs before any event is consumed, so fullDocument always reflects the
    // final state (c=kNumUpdates), regardless of which update the event is attached to.
    function expectedChange(c) {
        return {
            documentKey: {_id: 1},
            fullDocument: {_id: 1, c: kNumUpdates},
            ns: {db: db.getName(), coll: collName},
            operationType: "update",
            updateDescription: {removedFields: [], updatedFields: {c}, truncatedArrays: []},
        };
    }

    beforeEach(function () {
        coll = assertDropAndRecreateCollection(db, collName);
        assert.commandWorked(coll.insert({_id: 1, c: 0}));
    });

    afterEach(function () {
        assertDropCollection(db, collName);
    });

    it("reports the last emitted token at a batch boundary, so resume replays buffered events", function () {
        withChangeStreamTest(db, function (cst) {
            const cursor = cst.startWatchingChanges({
                pipeline: [{$changeStream: {fullDocument: "updateLookup"}}],
                collection: coll,
                querySettings: {queryKnobs: {changeStreamUpdateLookupMaxBatchSize: kMaxBatchSize}},
            });

            let expectedChanges = [];
            for (let i = 1; i <= kNumUpdates; ++i) {
                assert.commandWorked(coll.update({_id: 1}, {$set: {c: i}}));
                expectedChanges.push(expectedChange(i));
            }

            // If this test is running with secondary read preference, updateLookup's local
            // read has no causal-consistency guarantee: it must wait for the last update to
            // propagate to and be majority-committed on all secondaries, or the lookup can
            // observe a state older than 'kNumUpdates'.
            FixtureHelpers.awaitLastOpCommitted(db);

            // Consumes exactly the first event via a single getMore, well short of all
            // 'kNumUpdates' events the stage may have already buffered internally.
            cst.assertNextChangesEqual({
                cursor,
                expectedChanges: [expectedChanges[0]],
            });

            // The real safety property: resuming from the boundary PBRT replays every
            // unreturned event, none skipped or duplicated.
            cst.restartChangeStream(cursor);
            cst.assertNextChangesEqual({cursor, expectedChanges: expectedChanges.slice(1)});
        });
    });

    it("advances the resume token once batched-but-non-matching events are exhausted", function () {
        withChangeStreamTest(db, function (cst) {
            // Only the first update (c=1) satisfies this filter. 'fullDocument:
            // "updateLookup"' still forces a lookup on every one of the kNumUpdates events
            // regardless of $match, so updateLookup stage buffers and enriches all of
            // them together; $match then discards every one but the first.
            const cursor = cst.startWatchingChanges({
                pipeline: [
                    {$changeStream: {fullDocument: "updateLookup"}},
                    {$match: {"updateDescription.updatedFields.c": 1}},
                ],
                collection: coll,
                querySettings: {queryKnobs: {changeStreamUpdateLookupMaxBatchSize: kMaxBatchSize}},
            });

            for (let i = 1; i <= kNumUpdates; ++i) {
                assert.commandWorked(coll.update({_id: 1}, {$set: {c: i}}));
            }

            // If this test is running with secondary read preference, updateLookup's local
            // read has no causal-consistency guarantee: it must wait for the last update to
            // propagate to and be majority-committed on all secondaries, or the lookup can
            // observe a state older than 'kNumUpdates'.
            FixtureHelpers.awaitLastOpCommitted(db);

            const endTime = getClusterTime(db);

            // Consumes the single matching event. By this point updateLookup has
            // already scanned, enriched, and handed the remaining kNumUpdates - 1 events to
            // $match, which discarded them without ever surfacing them individually.
            cst.assertNextChangesEqual({cursor, expectedChanges: [expectedChange(1)]});

            // Once the stream has genuinely run out of new data, the postBatchResumeToken
            // must still be able to advance past the events that were scanned-and-discarded
            // alongside the one real match, not stay frozen at that match's own position forever.
            assert.soon(() => {
                cst.getNextBatch(cursor);
                const tokenTime = decodeResumeToken(cst.getResumeToken(cursor)).clusterTime;
                return bsonWoCompare(tokenTime, endTime) >= 0;
            }, "resume token never advanced past the batched-but-discarded events");
        });
    });
});
