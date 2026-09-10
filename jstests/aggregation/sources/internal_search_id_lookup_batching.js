/**
 * Verifies `$_internalSearchIdLookup` returns identical results regardless of which batching knob
 * forces a window boundary, for scalar and compound (object) `_id` shapes, and that each knob
 * forces the batch count it claims to (via the 'enrichBatchesStarted' serverStatus counter, since
 * correctness alone can't tell a working knob from a silently-ignored one). Also asserts the
 * per-engine found/notFound/notHandled counters accumulate correctly across multiple windows, not
 * just within a single lookup.
 *
 * @tags: [
 *   requires_fcv_90,
 *   # Runs $_internalSearchIdLookup directly over an internal-client connection; only mongod
 *   # accepts that. Every non-mongos-fronted resmoke suite already gives a bare mongod connection
 *   # as 'db', so this tag alone is sufficient.
 *   assumes_against_mongod_not_mongos,
 *   # The internal-client connection is marked internal by its hello; a read-preference override
 *   # would reroute the internal aggregates to a different, unmarked pooled connection, and the
 *   # server rejects internal-only stages from user connections.
 *   assumes_read_preference_unchanged,
 *   # Wrapping the mock-mongot pipelines in $facet breaks the result shapes.
 *   do_not_wrap_aggregations_in_facets,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {
    assertDropAndRecreateCollection,
    assertDropCollection,
} from "jstests/libs/collection_drop_recreate.js";
import {ServerStatusMetrics} from "jstests/libs/query/change_stream_metrics_util.js";
import {
    createInternalDB,
    runAggWithMockMongotResults,
} from "jstests/libs/query/internal_search_id_lookup_util.js";

const testDB = db.getSiblingDB(jsTestName());
const internalDB = createInternalDB(testDB.getMongo().uri, testDB.getName());

function idLookupMetricsDelta(fn) {
    const delta = ServerStatusMetrics.withServerStatusMetrics(testDB, fn);
    return (delta.search && delta.search.idLookup) || {};
}

function expectedBatchCount(numInputDocs, numFound, queryKnobs) {
    if (queryKnobs.searchIdLookupMaxOutputBytes === 1) {
        return numFound + 1;
    }
    if (queryKnobs.searchIdLookupMaxInputBytes === 1) {
        return numInputDocs;
    }
    return Math.ceil(numInputDocs / queryKnobs.searchIdLookupMaxBatchSize);
}

// Each config forces a window boundary via a different knob, with the other two left generous.
// 'byte-forced' configs keep maxBatchSize generous and squeeze the byte budget instead, so the
// boundary is hit via bytes rather than count.
const knobConfigs = [
    {name: "maxBatchSize=1", knobs: {searchIdLookupMaxBatchSize: 1}},
    {name: "maxBatchSize=10", knobs: {searchIdLookupMaxBatchSize: 10}},
    {name: "maxBatchSize=100", knobs: {searchIdLookupMaxBatchSize: 100}},
    {
        name: "maxInputBytes=1 (byte-forced batch-of-one)",
        knobs: {searchIdLookupMaxBatchSize: 100, searchIdLookupMaxInputBytes: 1},
    },
    {
        name: "maxOutputBytes=1 (byte-forced batch-of-one)",
        knobs: {searchIdLookupMaxBatchSize: 100, searchIdLookupMaxOutputBytes: 1},
    },
];

// Doubling each source `_id` (1..numDocs) yields lookup targets 2,4,...,2*numDocs; only the even
// targets that exist in the collection (2..numDocs) are found, the higher ones are dropped.
const numDocs = 24;
const compoundId = (seed) => ({a: seed, b: seed % 3});

describe("$_internalSearchIdLookup with scalar _id", function () {
    const collName = jsTestName() + "_scalar";

    const expectedFoundDocs = Array.from({length: numDocs / 2}, (_, i) => {
        const id = (i + 1) * 2;
        return {_id: id, x: "doc" + id};
    });

    // Ascends in the order expectedFoundDocs expects, so no downstream $sort is needed.
    const lookupIds = Array.from({length: numDocs}, (_, i) => (i + 1) * 2);
    const insertedDocs = Array.from({length: numDocs}, (_, i) => ({
        _id: i + 1,
        x: "doc" + (i + 1),
    }));

    before(function () {
        const coll = assertDropAndRecreateCollection(testDB, collName);
        assert.writeOK(coll.insert(insertedDocs));
    });

    after(function () {
        assertDropCollection(testDB, collName);
    });

    for (const {name, knobs} of knobConfigs) {
        // Scalar _id is always handled by SBE directly, so every outcome lands in the sbe cell.
        it(`returns identical results and opens the expected number of batches [${name}]`, function () {
            let actual;
            const idLookup = idLookupMetricsDelta(() => {
                actual = runAggWithMockMongotResults(internalDB, collName, lookupIds, {
                    queryKnobs: knobs,
                });
            });
            assert.eq(expectedFoundDocs, actual, {knobs});

            assert.eq(
                expectedBatchCount(numDocs, expectedFoundDocs.length, knobs),
                idLookup.enrichBatchesStarted,
                {knobs, idLookup},
            );
            const sbe = idLookup.sbe || {};
            const aggregation = idLookup.aggregation || {};
            assert.eq(sbe.found, expectedFoundDocs.length, {sbe, aggregation});
            assert.eq(sbe.notFound, numDocs - expectedFoundDocs.length, {sbe, aggregation});
            assert.eq(sbe.notHandled, 0, {sbe, aggregation});
            assert.eq(aggregation.found + aggregation.notFound, 0, {sbe, aggregation});
        });

        it(`honors idLookupSpec.limit under every batching knob, stopping the pull early rather than truncating downstream [${name}]`, function () {
            let limited;
            const idLookup = idLookupMetricsDelta(() => {
                limited = runAggWithMockMongotResults(internalDB, collName, lookupIds, {
                    idLookupSpec: {limit: 2},
                    queryKnobs: knobs,
                });
            });
            assert.eq(expectedFoundDocs.slice(0, 2), limited, {knobs});

            const sbe = idLookup.sbe || {};
            assert.eq(sbe.found, 2, {sbe});
            assert.eq(sbe.notFound, 0, {sbe});
            assert.eq(sbe.notHandled, 0, {sbe});
        });
    }

    // Mixes one document whose enriched size alone exceeds maxOutputBytes into an otherwise
    // normal batch, and confirms it (and every sibling around it) still resolves correctly.
    it("resolves a single oversized document as its own batch-of-one without breaking its neighbors", function () {
        const oversizedCollName = collName + "_oversized";
        const oversizedColl = assertDropAndRecreateCollection(testDB, oversizedCollName);

        const bigField = "x".repeat(1024);
        const docs = [
            {_id: 1, x: "small"},
            {_id: 2, x: "small"},
            {_id: 3, x: bigField},
            {_id: 4, x: "small"},
            {_id: 5, x: "small"},
        ];
        assert.writeOK(oversizedColl.insert(docs));

        // maxOutputBytes: 200 sits strictly between one and two minimal enriched documents' size
        // here (measured empirically), so two small documents merge into one batch but a third
        // can't join, and the oversized document always closes its batch alone. That gives
        // {1,2}, {3}, {4,5}: a 2,1,2 split that demonstrates the oversized document forcing a
        // break, rather than a cap small enough to isolate every document regardless of size.
        const knobs = {searchIdLookupMaxBatchSize: 100, searchIdLookupMaxOutputBytes: 200};
        let actual;
        const idLookup = idLookupMetricsDelta(() => {
            actual = runAggWithMockMongotResults(internalDB, oversizedCollName, [1, 2, 3, 4, 5], {
                queryKnobs: knobs,
            });
        });
        assert.eq(docs, actual);

        assert.eq(3, idLookup.enrichBatchesStarted, {idLookup});
        const sbe = idLookup.sbe || {};
        assert.eq(sbe.found, docs.length, {sbe});
        assert.eq(sbe.notFound, 0, {sbe});
        assert.eq(sbe.notHandled, 0, {sbe});
    });
});

// Same coverage, against a compound (object) `_id`, e.g. the kind of key used by
// config.system.preimages or any user collection with a compound _id.
describe("$_internalSearchIdLookup with compound (object) _id", function () {
    const collName = jsTestName() + "_object";

    // Looking up compoundId(seed*2) finds a document exactly when that doubled seed is itself a
    // stored seed, the same doubling structure as the scalar block above.
    const expectedFoundDocs = Array.from({length: numDocs / 2}, (_, i) => {
        const seed = (i + 1) * 2;
        return {_id: compoundId(seed), seed: seed, x: "doc" + seed};
    });

    const lookupIds = Array.from({length: numDocs}, (_, i) => compoundId((i + 1) * 2));
    const insertedDocs = Array.from({length: numDocs}, (_, i) => {
        const seed = i + 1;
        return {_id: compoundId(seed), seed: seed, x: "doc" + seed};
    });

    before(function () {
        const coll = assertDropAndRecreateCollection(testDB, collName);
        assert.writeOK(coll.insert(insertedDocs));
    });

    after(function () {
        assertDropCollection(testDB, collName);
    });

    for (const {name, knobs} of knobConfigs) {
        // TODO: SERVER-134080 Support non-scalar _id lookups in SbeSingleDocumentLookupExecutor.
        // Until then, every lookup here declines to the aggregation fallback, across every window.
        it(`returns identical results and opens the expected number of batches, all declined to aggregation [${name}]`, function () {
            let actual;
            const idLookup = idLookupMetricsDelta(() => {
                actual = runAggWithMockMongotResults(internalDB, collName, lookupIds, {
                    queryKnobs: knobs,
                });
            });
            assert.eq(expectedFoundDocs, actual);

            assert.eq(
                expectedBatchCount(numDocs, expectedFoundDocs.length, knobs),
                idLookup.enrichBatchesStarted,
                {knobs, idLookup},
            );
            const sbe = idLookup.sbe || {};
            const aggregation = idLookup.aggregation || {};
            assert.eq(sbe.notHandled, numDocs, {sbe});
            assert.eq(sbe.found, 0, {sbe});
            assert.eq(sbe.notFound, 0, {sbe});
            assert.eq(aggregation.found, expectedFoundDocs.length, {aggregation});
            assert.eq(aggregation.notFound, numDocs - expectedFoundDocs.length, {aggregation});
        });

        // Compound _id always declines, so the "stopped early" proof lives in aggregation, not sbe.
        it(`honors idLookupSpec.limit under every batching knob, stopping the pull early rather than truncating downstream [${name}]`, function () {
            let limited;
            const idLookup = idLookupMetricsDelta(() => {
                limited = runAggWithMockMongotResults(internalDB, collName, lookupIds, {
                    idLookupSpec: {limit: 2},
                    queryKnobs: knobs,
                });
            });
            assert.eq(expectedFoundDocs.slice(0, 2), limited);

            const aggregation = idLookup.aggregation || {};
            assert.eq(aggregation.found, 2, {aggregation});
            assert.eq(aggregation.notFound, 0, {aggregation});
        });
    }
});

// Neither block above touches both cells in the same query: scalar only writes sbe, compound
// only writes aggregation. This is the only place proving the two cells accumulate correctly at
// the same time, across multiple windows, rather than in isolation.
describe("$_internalSearchIdLookup with mixed scalar/compound _id in the same batch", function () {
    const collName = jsTestName() + "_mixed";

    // TODO: SERVER-134080 Support non-scalar _id lookups in SbeSingleDocumentLookupExecutor.
    // Until then, the compound docs below always decline, making this a genuine sbe/aggregation
    // mix rather than an all-sbe batch. Every window is a mixed pair by construction: one scalar
    // id, one compound id, both found.
    const docs = [
        {_id: 1, x: "doc1"},
        {_id: compoundId(2), x: "doc2"},
        {_id: 3, x: "doc3"},
        {_id: compoundId(4), x: "doc4"},
    ];
    const lookupIds = docs.map((d) => d._id);

    const coll = assertDropAndRecreateCollection(testDB, collName);
    assert.writeOK(coll.insert(docs));

    it("accumulates into both the sbe and aggregation cells without cross-contaminating each other across repeated windows", function () {
        let actual;
        const idLookup = idLookupMetricsDelta(() => {
            actual = runAggWithMockMongotResults(internalDB, collName, lookupIds, {
                queryKnobs: {searchIdLookupMaxBatchSize: 2},
            });
        });
        assert.eq(docs, actual);
        assert.eq(2, idLookup.enrichBatchesStarted, {idLookup});

        // The 2 compound docs decline: sbe.notHandled records the decline, and the aggregation
        // fallback records its own found outcome for the same 2 documents.
        const sbe = idLookup.sbe || {};
        const aggregation = idLookup.aggregation || {};
        assert.eq(sbe.found, 2, {sbe});
        assert.eq(sbe.notFound, 0, {sbe});
        assert.eq(sbe.notHandled, 2, {sbe});
        assert.eq(aggregation.found, 2, {aggregation});
        assert.eq(aggregation.notFound, 0, {aggregation});
    });
});
