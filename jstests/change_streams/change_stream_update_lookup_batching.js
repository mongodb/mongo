/**
 * Verifies updateLookup returns identical events regardless of which of the three independent
 * batching knobs is set ('internalChangeStreamUpdateLookupMaxBatchSize'/'MaxInputBytes'/
 * 'MaxOutputBytes'), and that each knob actually forces the batch count it claims to (via the
 * 'enrichBatchesStarted' serverStatus counter, since correctness alone can't tell a working knob
 * from a silently-ignored one). Also asserts the per-engine found/notFound counters accumulate
 * correctly across multiple windows, not just within a single lookup: a bug that double-counts or
 * drops an increment at a window boundary would be invisible to correctness or batch-count checks
 * alone.
 *
 * @tags: [
 *   requires_fcv_90,
 *   uses_change_streams,
 *   # The exact batch-count and metrics-delta assertions no longer hold once the txn
 *   # passthrough bundles the test's writes into transactions.
 *   change_stream_does_not_expect_txns,
 *   # The exact enrich-batch-count assertions model primary-read timing: the first getMore
 *   # always forms a batch-of-one and the remaining events arrive together in one window.
 *   # Under secondary reads the window boundaries depend on when events replicate to the
 *   # serving node (observed 3 batches where 2 were expected on the disagg secondary-reads
 *   # burn-in), and through mongos the drain can even observe early cursor EOF. The knobs
 *   # are per-command querySettings, so secondary-read coverage of them adds nothing.
 *   assumes_read_preference_unchanged,
 *   # The per-shard-cursor passthroughs implicitly shard accessed collections, which can
 *   # distribute this test's updates across shards: both the positional event comparisons
 *   # and the exact enrich-batch counts are nondeterministic in that topology.
 *   assumes_unsharded_collection,
 * ]
 */
import {afterEach, describe, it} from "jstests/libs/mochalite.js";
import {assertChangeStreamEventEq} from "jstests/libs/query/change_stream_util.js";
import {
    expectedUpdateLookupEngine,
    ServerStatusMetrics,
    UpdateLookupExecutor,
} from "jstests/libs/query/change_stream_metrics_util.js";

// Each config forces the window boundary via a different knob, leaving the other two generous.
const knobConfigs = [
    {name: "maxBatchSize=1", queryKnobs: {changeStreamUpdateLookupMaxBatchSize: 1}},
    {name: "maxBatchSize=10", queryKnobs: {changeStreamUpdateLookupMaxBatchSize: 10}},
    {name: "maxBatchSize=100", queryKnobs: {changeStreamUpdateLookupMaxBatchSize: 100}},
    {
        name: "maxInputBytes=1 (byte-forced batch-of-one)",
        queryKnobs: {
            changeStreamUpdateLookupMaxBatchSize: 100,
            changeStreamUpdateLookupMaxInputBytes: 1,
        },
    },
    {
        name: "maxOutputBytes=1 (byte-forced batch-of-one)",
        queryKnobs: {
            changeStreamUpdateLookupMaxBatchSize: 100,
            changeStreamUpdateLookupMaxOutputBytes: 1,
        },
    },
];

describe("change stream updateLookup batching-knob invariance", function () {
    const testDB = db.getSiblingDB(jsTestName());
    const collName = jsTestName();
    const testColl = testDB.getCollection(collName);
    const ns = {db: testDB.getName(), coll: collName};
    const numDocs = 11;
    const ids = Array.from({length: numDocs}, (_, i) => i);

    afterEach(function () {
        assert.commandWorked(testDB.dropDatabase());
    });

    // Deletes also pass through enrich() untouched, so they occupy a batch slot too.
    const numGoneIds = Math.floor(numDocs / 2);
    const totalEnrichEvents = numDocs + numGoneIds;

    // Batching is a property of the SBE primary (collection-level streams), not of the
    // collection.
    const engine = expectedUpdateLookupEngine();
    const isBatchingEligible = engine === UpdateLookupExecutor.kSBE;

    // '+1' accounts for a fresh awaitData cursor's first event always forming its own batch-of-one
    // (fillBatch()'s shouldWaitForInserts check), before the remaining events batch normally.
    function expectedBatchCount(queryKnobs) {
        if (!isBatchingEligible) {
            return totalEnrichEvents;
        }
        if (
            queryKnobs.changeStreamUpdateLookupMaxInputBytes === 1 ||
            queryKnobs.changeStreamUpdateLookupMaxOutputBytes === 1
        ) {
            return totalEnrichEvents;
        }
        return (
            1 + Math.ceil((totalEnrichEvents - 1) / queryKnobs.changeStreamUpdateLookupMaxBatchSize)
        );
    }

    function updateLookupMetricsDelta(fn) {
        const delta = ServerStatusMetrics.withServerStatusMetricsAcrossCluster(testDB, fn);
        return (delta.changeStreams && delta.changeStreams.updateLookup) || {};
    }

    // A change stream cursor never signals EOF, so drain exactly 'expectedCount' events.
    function drainAllChanges(cursor, expectedCount) {
        const changes = [];
        for (let i = 0; i < expectedCount; i++) {
            changes.push(cursor.next());
        }
        return changes;
    }

    for (const {name, queryKnobs} of knobConfigs) {
        it(`returns identical events and opens the expected number of batches [${name}]`, function () {
            assert.commandWorked(testColl.insert(ids.map((id) => ({_id: id}))));

            let actualChanges;
            const delta = updateLookupMetricsDelta(() => {
                const cursor = testColl.watch([], {
                    fullDocument: "updateLookup",
                    cursor: {batchSize: 0},
                    querySettings: {queryKnobs},
                });

                // Alternating found/gone outcomes ensure every window is a mixed one.
                const expectedChanges = [];
                for (const id of ids) {
                    const shouldRemove = id % 2 === 1;

                    assert.commandWorked(testColl.update({_id: id}, {$set: {v: 1}}));
                    expectedChanges.push({
                        operationType: "update",
                        ns,
                        documentKey: {_id: id},
                        fullDocument: shouldRemove ? null : {_id: id, v: 1},
                    });

                    if (shouldRemove) {
                        assert.commandWorked(testColl.remove({_id: id}));
                        expectedChanges.push({operationType: "delete", ns, documentKey: {_id: id}});
                    }
                }

                actualChanges = drainAllChanges(cursor, expectedChanges.length);
                for (let i = 0; i < expectedChanges.length; i++) {
                    assertChangeStreamEventEq(actualChanges[i], expectedChanges[i]);
                }
                cursor.close();
            });

            assert.eq(expectedBatchCount(queryKnobs), delta.enrichBatchesStarted, {
                queryKnobs,
                delta,
            });

            const lookup = delta[engine] || {};
            const numGone = ids.filter((id) => id % 2 === 1).length;
            assert.eq(lookup.found, numDocs - numGone, {lookup, delta});
            assert.eq(lookup.notFound, numGone, {lookup, delta});
            assert.eq(lookup.notHandled, 0, {lookup, delta});
        });
    }

    // enrichBatch() checks the byte cap *before* pulling the next event, so a fresh batch always
    // admits one event and a small event can drag in an oversized follower. With a minimal event
    // at ~2978 bytes, the 4096-char one at ~11191, and the cap at 4000 (between one and two
    // minimal events), the batches must be exactly {0}, {1,2}, {3}, {4,5}: small pairs merge, a
    // third can't join, and the oversized event always closes its batch alone.
    it("resolves a single oversized post-image as its own batch-of-one without breaking its neighbors", function () {
        assert.commandWorked(
            testColl.insert([{_id: 0}, {_id: 1}, {_id: 2}, {_id: 3}, {_id: 4}, {_id: 5}]),
        );

        const bigField = "x".repeat(4096);
        const updates = [
            {id: 0, v: 1},
            {id: 1, v: 1},
            {id: 2, v: 1},
            {id: 3, v: bigField},
            {id: 4, v: 1},
            {id: 5, v: 1},
        ];

        let actualChanges;
        let expectedChanges;
        const delta = updateLookupMetricsDelta(() => {
            const cursor = testColl.watch([], {
                fullDocument: "updateLookup",
                cursor: {batchSize: 0},
                querySettings: {
                    queryKnobs: {
                        changeStreamUpdateLookupMaxBatchSize: 100,
                        changeStreamUpdateLookupMaxOutputBytes: 4000,
                    },
                },
            });

            expectedChanges = updates.map(({id, v}) => {
                assert.commandWorked(testColl.update({_id: id}, {$set: {v}}));
                return {
                    operationType: "update",
                    ns,
                    documentKey: {_id: id},
                    fullDocument: {_id: id, v},
                };
            });

            actualChanges = drainAllChanges(cursor, expectedChanges.length);
            cursor.close();
        });

        for (let i = 0; i < expectedChanges.length; i++) {
            assertChangeStreamEventEq(actualChanges[i], expectedChanges[i]);
        }

        // {0}, {1,2}, {3}, {4,5}. Only when batching applies at all (the SBE primary); a
        // whole-db/whole-cluster stream runs Express, always batch-of-one, so every event opens
        // its own batch regardless of the byte cap.
        const expectedBatches = isBatchingEligible ? 4 : updates.length;
        assert.eq(expectedBatches, delta.enrichBatchesStarted, {delta});

        // Every update found its document; none removed here.
        const lookup = delta[engine] || {};
        assert.eq(lookup.found, updates.length, {lookup, delta});
        assert.eq(lookup.notFound, 0, {lookup, delta});
        assert.eq(lookup.notHandled, 0, {lookup, delta});
    });
});
