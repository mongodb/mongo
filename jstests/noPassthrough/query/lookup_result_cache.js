/**
 * Tests correctness of the classic $lookup LRU result cache, which caches foreign query results
 * by localField value for pure localField/foreignField equijoins. Covers eviction correctness
 * under a small total-size cap, per-entry-cap correctness, and collation-aware key matching.
 */
import {resultsEq} from "jstests/aggregation/extras/utils.js";
import {describe, it, before, after, beforeEach} from "jstests/libs/mochalite.js";

// Sets 'params' (a {name: value} map of setParameter names/values), runs 'fn', then restores each
// parameter to whatever value it held before this call so the reset stays correct even if a
// shipping default changes later.
function withParams(db, params, fn) {
    const original = {};
    for (const name of Object.keys(params)) {
        original[name] = assert.commandWorked(db.adminCommand({getParameter: 1, [name]: 1}))[name];
    }
    assert.commandWorked(db.adminCommand({setParameter: 1, ...params}));
    try {
        fn();
    } finally {
        assert.commandWorked(db.adminCommand({setParameter: 1, ...original}));
    }
}

describe("$lookup result cache", function () {
    before(function () {
        this.conn = MongoRunner.runMongod();
        this.db = this.conn.getDB("test");
        assert.commandWorked(
            this.db.adminCommand({
                setParameter: 1,
                internalQueryFrameworkControl: "forceClassicEngine",
            }),
        );
    });

    after(function () {
        MongoRunner.stopMongod(this.conn);
    });

    beforeEach(function () {
        this.db.local.drop();
        this.db.foreign.drop();
    });

    it("returns correct results when forced to evict under a small entry-count cap", function () {
        // Cap the cache to 2 entries, but query 5 distinct localField values, each more than
        // once, forcing repeated eviction.
        withParams(
            this.db,
            {
                internalLookupStageResultCacheMaxEntries: 2,
                internalLookupStageResultCacheMaxEntrySizeBytes: 100,
            },
            () => {
                assert.commandWorked(
                    this.db.foreign.insertMany([
                        {_id: 0, k: 0, v: "a"},
                        {_id: 1, k: 1, v: "b"},
                        {_id: 2, k: 2, v: "c"},
                        {_id: 3, k: 3, v: "d"},
                        {_id: 4, k: 4, v: "e"},
                    ]),
                );

                // Interleave local documents so the cache is repeatedly forced to evict and
                // re-fetch earlier keys (0..4 twice, in order).
                const localDocs = [];
                for (let round = 0; round < 2; ++round) {
                    for (let k = 0; k < 5; ++k) {
                        localDocs.push({local_k: k});
                    }
                }
                assert.commandWorked(this.db.local.insertMany(localDocs));

                const actual = this.db.local
                    .aggregate([
                        {
                            $lookup: {
                                from: "foreign",
                                localField: "local_k",
                                foreignField: "k",
                                as: "f",
                            },
                        },
                        {$project: {_id: 0, local_k: 1, f: 1}},
                    ])
                    .toArray();

                const expected = [];
                for (let round = 0; round < 2; ++round) {
                    for (let k = 0; k < 5; ++k) {
                        expected.push({local_k: k, f: [{_id: k, k: k, v: "abcde"[k]}]});
                    }
                }
                assert(
                    resultsEq(expected, actual),
                    () => "Expected: " + tojson(expected) + " but got: " + tojson(actual),
                );
            },
        );
    });

    it("returns correct results when entries exceed a tiny per-entry cap", function () {
        // A per-entry cap of 1 byte means every entry is too large to cache, so every lookup
        // falls back to executing the full pipeline.
        withParams(this.db, {internalLookupStageResultCacheMaxEntrySizeBytes: 1}, () => {
            assert.commandWorked(
                this.db.foreign.insertMany([
                    {_id: 0, k: 0, v: "a"},
                    {_id: 1, k: 1, v: "b"},
                ]),
            );
            assert.commandWorked(
                this.db.local.insertMany([{local_k: 0}, {local_k: 0}, {local_k: 1}, {local_k: 0}]),
            );

            const actual = this.db.local
                .aggregate([
                    {
                        $lookup: {
                            from: "foreign",
                            localField: "local_k",
                            foreignField: "k",
                            as: "f",
                        },
                    },
                    {$project: {_id: 0, local_k: 1, f: 1}},
                ])
                .toArray();

            const expected = [
                {local_k: 0, f: [{_id: 0, k: 0, v: "a"}]},
                {local_k: 0, f: [{_id: 0, k: 0, v: "a"}]},
                {local_k: 1, f: [{_id: 1, k: 1, v: "b"}]},
                {local_k: 0, f: [{_id: 0, k: 0, v: "a"}]},
            ];
            assert(
                resultsEq(expected, actual),
                () => "Expected: " + tojson(expected) + " but got: " + tojson(actual),
            );
        });
    });

    it("returns correct results when the cache is disabled via a zero size cap", function () {
        // MaxEntries=0 (and separately MaxEntrySizeBytes=0) means the cache is never even
        // constructed; every lookup must fall back to executing the full pipeline.
        withParams(this.db, {internalLookupStageResultCacheMaxEntries: 0}, () => {
            assert.commandWorked(
                this.db.foreign.insertMany([
                    {_id: 0, k: 0, v: "a"},
                    {_id: 1, k: 1, v: "b"},
                ]),
            );
            assert.commandWorked(
                this.db.local.insertMany([{local_k: 0}, {local_k: 0}, {local_k: 1}]),
            );

            const actual = this.db.local
                .aggregate([
                    {$lookup: {from: "foreign", localField: "local_k", foreignField: "k", as: "f"}},
                    {$project: {_id: 0, local_k: 1, f: 1}},
                ])
                .toArray();

            const expected = [
                {local_k: 0, f: [{_id: 0, k: 0, v: "a"}]},
                {local_k: 0, f: [{_id: 0, k: 0, v: "a"}]},
                {local_k: 1, f: [{_id: 1, k: 1, v: "b"}]},
            ];
            assert(
                resultsEq(expected, actual),
                () => "Expected: " + tojson(expected) + " but got: " + tojson(actual),
            );
        });

        this.db.local.drop();
        this.db.foreign.drop();
        withParams(this.db, {internalLookupStageResultCacheMaxEntrySizeBytes: 0}, () => {
            assert.commandWorked(this.db.foreign.insertMany([{_id: 0, k: 0, v: "a"}]));
            assert.commandWorked(this.db.local.insertMany([{local_k: 0}, {local_k: 0}]));

            const actual = this.db.local
                .aggregate([
                    {$lookup: {from: "foreign", localField: "local_k", foreignField: "k", as: "f"}},
                    {$project: {_id: 0, local_k: 1, f: 1}},
                ])
                .toArray();

            const expected = [
                {local_k: 0, f: [{_id: 0, k: 0, v: "a"}]},
                {local_k: 0, f: [{_id: 0, k: 0, v: "a"}]},
            ];
            assert(
                resultsEq(expected, actual),
                () => "Expected: " + tojson(expected) + " but got: " + tojson(actual),
            );
        });
    });

    it("returns correct results after the fast bailout disables the cache mid-query", function () {
        // 3 consecutive misses on distinct keys trip the fast bailout, well before the default
        // warmup window would ever close, disabling the cache for the remainder of the query.
        withParams(this.db, {internalLookupStageResultCacheFastBailoutMisses: 3}, () => {
            assert.commandWorked(
                this.db.foreign.insertMany([
                    {_id: 0, k: 0, v: "a"},
                    {_id: 1, k: 1, v: "b"},
                    {_id: 2, k: 2, v: "c"},
                    {_id: 3, k: 3, v: "d"},
                ]),
            );
            // The first 3 localField values are all distinct misses, tripping the bailout; the
            // 4th repeats an earlier value and must still return correct (uncached) results.
            assert.commandWorked(
                this.db.local.insertMany([{local_k: 0}, {local_k: 1}, {local_k: 2}, {local_k: 0}]),
            );

            const actual = this.db.local
                .aggregate([
                    {$lookup: {from: "foreign", localField: "local_k", foreignField: "k", as: "f"}},
                    {$project: {_id: 0, local_k: 1, f: 1}},
                ])
                .toArray();

            const expected = [
                {local_k: 0, f: [{_id: 0, k: 0, v: "a"}]},
                {local_k: 1, f: [{_id: 1, k: 1, v: "b"}]},
                {local_k: 2, f: [{_id: 2, k: 2, v: "c"}]},
                {local_k: 0, f: [{_id: 0, k: 0, v: "a"}]},
            ];
            assert(
                resultsEq(expected, actual),
                () => "Expected: " + tojson(expected) + " but got: " + tojson(actual),
            );
        });
    });

    it("returns correct results after the warmup/hit-rate backoff disables the cache", function () {
        // Warmup of 2 lookups with a 100% hit-rate floor: the 2nd lookup (a miss on a
        // never-repeating key) closes the window at a 50% hit rate, below the floor, disabling
        // the cache for the remainder of the query.
        withParams(
            this.db,
            {
                internalLookupStageResultCacheWarmupSize: 2,
                internalLookupStageResultCacheMinHitPercent: 100,
            },
            () => {
                assert.commandWorked(
                    this.db.foreign.insertMany([
                        {_id: 0, k: 0, v: "a"},
                        {_id: 1, k: 1, v: "b"},
                        {_id: 2, k: 2, v: "c"},
                    ]),
                );
                // local_k=0 repeats (a hit the 2nd time it's seen), local_k=1/2 are each seen
                // once; by the time the 2nd lookup (local_k=0's repeat) closes the warmup
                // window, the hit rate is below the 100% floor and the cache disables itself.
                assert.commandWorked(
                    this.db.local.insertMany([
                        {local_k: 0},
                        {local_k: 0},
                        {local_k: 1},
                        {local_k: 2},
                    ]),
                );

                const actual = this.db.local
                    .aggregate([
                        {
                            $lookup: {
                                from: "foreign",
                                localField: "local_k",
                                foreignField: "k",
                                as: "f",
                            },
                        },
                        {$project: {_id: 0, local_k: 1, f: 1}},
                    ])
                    .toArray();

                const expected = [
                    {local_k: 0, f: [{_id: 0, k: 0, v: "a"}]},
                    {local_k: 0, f: [{_id: 0, k: 0, v: "a"}]},
                    {local_k: 1, f: [{_id: 1, k: 1, v: "b"}]},
                    {local_k: 2, f: [{_id: 2, k: 2, v: "c"}]},
                ];
                assert(
                    resultsEq(expected, actual),
                    () => "Expected: " + tojson(expected) + " but got: " + tojson(actual),
                );
            },
        );
    });

    it("matches cache keys with case-insensitive collation", function () {
        assert.commandWorked(
            this.db.createCollection("local", {collation: {locale: "en", strength: 2}}),
        );
        assert.commandWorked(
            this.db.createCollection("foreign", {collation: {locale: "en", strength: 2}}),
        );

        assert.commandWorked(
            this.db.foreign.insertMany([
                {_id: 0, k: "Hat", v: 4.5},
                {_id: 1, k: "hat", v: 4},
            ]),
        );
        // "hat", "HAT", and "Hat" should all be treated as the same cache key under the
        // case-insensitive collation, and should match both foreign documents.
        assert.commandWorked(
            this.db.local.insertMany([{local_k: "hat"}, {local_k: "HAT"}, {local_k: "Hat"}]),
        );

        const actual = this.db.local
            .aggregate([
                {$lookup: {from: "foreign", localField: "local_k", foreignField: "k", as: "f"}},
                {$project: {_id: 0, local_k: 1, "f.v": 1}},
            ])
            .toArray();

        for (const doc of actual) {
            const stars = doc.f.map((r) => r.v).sort();
            assert.eq(stars, [4, 4.5], () => "Unexpected foreign matches for " + tojson(doc));
        }
        assert.eq(actual.length, 3);
    });

    it("does not cache results when the foreign namespace is a non-deterministic view", function () {
        // A view's underlying pipeline could be non-deterministic (e.g. $rand), so the cache must
        // never serve stale results for it -- confirmed here end-to-end, not just via the
        // resultCacheEnabled_forTest() unit test.
        this.db.foreignView.drop();
        assert.commandWorked(
            this.db.createView("foreignView", "foreign", [{$addFields: {k: 0, r: {$rand: {}}}}]),
        );
        assert.commandWorked(this.db.foreign.insertMany([{}]));

        // Repeat the same localField value so a caching bug would return identical 'r' values.
        assert.commandWorked(this.db.local.insertMany([{local_k: 0}, {local_k: 0}, {local_k: 0}]));

        const actual = this.db.local
            .aggregate([
                {$lookup: {from: "foreignView", localField: "local_k", foreignField: "k", as: "f"}},
                {$project: {_id: 0, r: {$arrayElemAt: ["$f.r", 0]}}},
            ])
            .toArray();

        assert.eq(actual.length, 3);
        const randValues = new Set(actual.map((doc) => doc.r));
        assert.eq(
            randValues.size,
            3,
            () =>
                "Expected distinct $rand values, cache may be serving stale results: " +
                tojson(actual),
        );
    });
});
