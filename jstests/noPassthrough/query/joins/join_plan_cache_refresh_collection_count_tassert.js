/**
 * Reproduces SERVER-134859: a tripwire assertion in JoinPlanCacheEntry::refreshCollectionTags.
 *
 * The join plan cache is keyed on the join graph only, but each entry stores a CollectionTag for
 * every collection in the query's MultipleCollectionAccessor (see 'makeCollectionTags()'), which
 * includes all $lookup foreign namespaces, even $lookups the join optimizer did NOT pushdown into
 * the join graph.
 *
 * So two pipelines can produce the same cache key (identical join graph) while carrying a different
 * number of collections in their accessor. On a hit, when a benign DDL has bumped a collection
 * version, 'validateCacheEntry()' revalidates the entry and calls 'refreshCollectionTags()' with a
 * CollectionTag vector of a different length than the entry's, causing a tassert.
 *
 * @tags: [
 *   requires_fcv_91,
 *   requires_sbe,
 * ]
 */

import {afterEach, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {assertJoinPlanCacheStats} from "jstests/libs/query/join_utils.js";

describe("join plan cache refresh collection count tassert", function () {
    beforeEach(function () {
        this.conn = MongoRunner.runMongod({
            setParameter: {
                internalEnableJoinOptimization: true,
                internalEnableJoinPlanCache: true,
            },
        });

        const db = this.conn.getDB(jsTestName());
        this.db = db;
        this.adminDB = this.conn.getDB("admin");
        this.base = db[jsTestName()];
        this.foreign = db[jsTestName() + "_a"];
        this.extra = db[jsTestName() + "_extra"];

        assert.commandWorked(this.base.insertMany([{a: 1}, {a: 1}, {a: 2}, {a: 2}]));
        assert.commandWorked(this.base.createIndex({dummy: 1, a: 1}));

        assert.commandWorked(this.foreign.insertMany([{a: 1}, {a: 1}, {a: 2}, {a: 2}]));
        assert.commandWorked(this.foreign.createIndex({dummy: 1, a: 1}));

        assert.commandWorked(this.extra.insertMany([{a: 1}, {a: 2}]));
        // No index on 'extra' so a join on extra is not join eligible.
    });

    afterEach(function () {
        MongoRunner.stopMongod(this.conn);
    });

    it("does not trip the tassert when DDL bumps a collection's version on a cache hit", function () {
        // Pipeline 1: join base and foreign and assert on the number of resulting rows.
        const q1 = [
            {
                $lookup: {
                    from: this.foreign.getName(),
                    localField: "a",
                    foreignField: "a",
                    as: "foreign",
                },
            },
            {$unwind: "$foreign"},
        ];
        assertJoinPlanCacheStats({
            db: this.db,
            fn: () => assert.eq(this.base.aggregate(q1).itcount(), 8, "Q1 expected 8 rows"),
            expectedHits: 0,
            expectedMisses: 1,
        });

        // Benign DDL: an index on a field no predicate references. It bumps the collection version
        // but leaves every node's relevant-index fingerprint intact, so the next hit must be
        // revalidated, exercising 'refreshCollectionTags()'.
        assert.commandWorked(this.foreign.createIndex({e: 1}));

        // Pipeline 2: same join graph as pipeline 1 with a trailing $lookup that is not join
        // eligible referencing the "extra" collection.
        const q2 = [
            ...q1,
            {
                $lookup: {
                    from: this.extra.getName(),
                    localField: "a",
                    foreignField: "a",
                    as: "extra",
                },
            },
        ];

        // Before SERVER-134859, this triggered a tassert. After the fix, this query should succeed.
        assertJoinPlanCacheStats({
            db: this.db,
            fn: () => assert.eq(this.base.aggregate(q2).itcount(), 8, "Q2 expected 8 rows"),
            expectedHits: 1,
            expectedMisses: 0,
        });
    });

    it("creates a new cache entry when an index makes the second lookup join-eligible", function () {
        // Two $lookup/$unwind pairs. The first lookup's collection has an index on the join field
        // 'a', so it is join eligible. The second lookup's collection initially has no such index,
        // so that join is not eligible.
        const twoLookups = [
            {
                $lookup: {
                    from: this.foreign.getName(),
                    localField: "a",
                    foreignField: "a",
                    as: "foreign",
                },
            },
            {$unwind: "$foreign"},
            {
                $lookup: {
                    from: this.extra.getName(),
                    localField: "a",
                    foreignField: "a",
                    as: "extra",
                },
            },
            {$unwind: "$extra"},
        ];
        const run = () => this.base.aggregate(twoLookups).itcount();

        // Prime the cache: the first run misses, the second hits.
        assertJoinPlanCacheStats({db: this.db, fn: run, expectedHits: 0, expectedMisses: 1});
        assertJoinPlanCacheStats({db: this.db, fn: run, expectedHits: 1, expectedMisses: 0});

        // Adding an index on the second lookup's join field makes it join-eligible.
        assert.commandWorked(this.extra.createIndex({dummy: 1, a: 1}));

        assertJoinPlanCacheStats({
            db: this.db,
            fn: () => assert.eq(run(), 8, "expected 8 rows after index creation"),
            expectedHits: 0,
            expectedMisses: 1,
        });
        assertJoinPlanCacheStats({
            db: this.db,
            fn: () => assert.eq(run(), 8, "expected 8 rows on re-cached entry"),
            expectedHits: 1,
            expectedMisses: 0,
        });

        // Assert the cache contains two entries, one for both query shapes.
        const cacheEntries = this.adminDB.aggregate([{$joinPlanCacheStats: {}}]).toArray();
        assert.eq(2, cacheEntries.length, cacheEntries);
    });
});
