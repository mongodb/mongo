/**
 * End to end test that DDL operations invalidate cached join plans. Verifies via server logs that
 * after a join query shape has been cached (and is being served from the cache), a DDL operation
 * (index create/drop) on a referenced collection forces the next identical query to miss the cache
 * and re-optimize, because the DDL bumps the collection's version tag.
 *
 * TODO(SERVER-129272): Implement this test without relying on server logs once we have commands
 * to inspect the join plan cache.
 *
 * @tags: [
 *   requires_fcv_90,
 *   requires_sbe,
 * ]
 */

import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {assertAllJoinsUseMethod} from "jstests/libs/query/join_utils.js";

const JOIN_PLAN_CACHE_HIT_LOG_ID = 11083906;
const JOIN_PLAN_CACHE_MISS_LOG_ID = 11083907;

// Counts occurrences of each given structured log ID in a single pass over the log file. Returns an
// object mapping each id to its count.
function countLogIds(logFile, ids) {
    const counts = {};
    for (const id of ids) {
        counts[id] = 0;
    }
    for (const line of cat(logFile).split("\n")) {
        if (line.length === 0) {
            continue;
        }
        let entry;
        try {
            entry = JSON.parse(line);
        } catch (e) {
            continue;
        }
        if (entry && counts.hasOwnProperty(entry.id)) {
            counts[entry.id]++;
        }
    }
    return counts;
}

describe("join plan cache DDL invalidation", function () {
    // Test-wide, created once in before(): the mongod stays up for the whole test.
    let conn;
    let db;
    let logFile;
    // Reset each test by resetCollections() (see beforeEach).
    let baseColl;
    let foreignColl;
    let pipeline;
    let runSpec;

    before(function () {
        conn = MongoRunner.runMongod({
            useLogFiles: true,
            setParameter: {
                internalEnableJoinOptimization: true,
                internalEnableJoinPlanCache: true,
            },
        });
        logFile = conn.fullOptions.logFile;
        db = conn.getDB(jsTestName());

        // Raise query log verbosity so the join plan cache hit/miss log lines are emitted.
        assert.commandWorked(conn.getDB("admin").setLogLevel(5, "query"));
    });

    after(function () {
        MongoRunner.stopMongod(conn);
    });

    // (Re)creates the foreign collection in a known clean state with a fresh UUID. Extracted so the
    // renameCollection test can recreate it at the same name (new UUID) to exercise UUID-mismatch
    // invalidation.
    function createForeignColl() {
        foreignColl = db[jsTestName() + "_a"];
        foreignColl.drop();
        assert.commandWorked(
            foreignColl.insertMany([
                {a: 1, c: "foo", d: 1},
                {a: 1, c: "bar", d: 2},
                {a: 2, c: "baz", d: 1},
                {a: 2, c: "qux", d: 2},
            ]),
        );
        // Index for multikeyness info for path arrayness.
        assert.commandWorked(foreignColl.createIndex({dummy: 1, a: 1, c: 1, d: 1}));
    }

    // (Re)creates the shared base + foreign collections in a known clean state and rebuilds the
    // shared runSpec. Called from beforeEach so every test starts from pristine collections (no
    // leftover indexes from a prior test). Because the recreated collections get fresh UUIDs, any
    // join plan cache entry a previous test left for this query shape is stale, so the first run of
    // each test deterministically misses.
    function resetCollections() {
        // TODO (SERVER-129272): Clear the join plan cache here instead of relying on fresh UUIDs.
        baseColl = db[jsTestName()];
        baseColl.drop();
        assert.commandWorked(
            baseColl.insertMany([
                {a: 1, b: 1, d: 1},
                {a: 1, b: 2, d: 2},
                {a: 2, b: 1, d: 1},
                {a: 2, b: 2, d: 2},
            ]),
        );
        // Index for multikeyness info for path arrayness.
        assert.commandWorked(baseColl.createIndex({dummy: 1, a: 1, b: 1, d: 1}));

        createForeignColl();

        pipeline = [
            {
                $lookup: {
                    from: foreignColl.getName(),
                    localField: "a",
                    foreignField: "a",
                    as: "foreignColl",
                },
            },
            {$unwind: "$foreignColl"},
        ];

        // The shared runSpec passed to runOnce() by the tests that use the base+foreign pair.
        runSpec = {logFile, coll: baseColl, pipeline, expectedResultCount: 8};
    }

    beforeEach(function () {
        resetCollections();
        // Reset the forced join method so a test that forces INLJ (and fails before resetting) can't
        // leak that setting into later tests.
        setForcedJoinMethod("any");
    });

    // Runs 'pipeline' on 'coll' once, asserts the result size, and returns whether
    // that run hit or missed the join plan cache: {hit, miss} as booleans, derived from the change
    // in the hit/miss log counts. A run that is ineligible for join optimization (e.g. a referenced
    // collection no longer exists) touches the cache for neither, so both are false.
    function runOnce({logFile, coll, pipeline, expectedResultCount}) {
        const ids = [JOIN_PLAN_CACHE_HIT_LOG_ID, JOIN_PLAN_CACHE_MISS_LOG_ID];
        const before = countLogIds(logFile, ids);

        assert.eq(coll.aggregate(pipeline).toArray().length, expectedResultCount);

        const after = countLogIds(logFile, ids);
        const hitDelta = after[JOIN_PLAN_CACHE_HIT_LOG_ID] - before[JOIN_PLAN_CACHE_HIT_LOG_ID];
        const missDelta = after[JOIN_PLAN_CACHE_MISS_LOG_ID] - before[JOIN_PLAN_CACHE_MISS_LOG_ID];
        assert.lte(hitDelta + missDelta, 1, "a single run cannot both hit and miss the cache", {
            hitDelta,
            missDelta,
        });
        return {hit: hitDelta === 1, miss: missDelta === 1};
    }

    // Primes the cache from a freshly reset collection state: the first run must miss and cache the
    // plan, the second must be served from the cache (hit). Leaves the entry warm for the DDL under
    // test. Relies on beforeEach's resetCollections making the first run a deterministic miss.
    function primeCache(runSpec) {
        assert.eq(runOnce(runSpec).miss, true, "first run should miss and cache the plan");
        assert.eq(runOnce(runSpec).hit, true, "second run should be served from the cache");
    }

    // Forces (or clears, with "any") the join method for all joins. This is a server knob, not a
    // query hint, so unlike a hint it does NOT disable the join plan cache.
    function setForcedJoinMethod(method) {
        assert.commandWorked(db.adminCommand({setParameter: 1, internalJoinMethod: method}));
    }

    // Asserts, via a separate explain (which bypasses the plan cache), that every join in the shared
    // pipeline's current plan is an indexed nested loop join -- i.e. it really uses the {a: 1} seek
    // index. Guards the "index used by the cached plan" tests: if the plan stops using INLJ (e.g.
    // heuristics change), this fails loudly instead of the test silently exercising an unused index.
    function assertPipelineUsesInlj() {
        assertAllJoinsUseMethod(baseColl.explain().aggregate(pipeline), "INLJ");
    }

    it("re-caches (misses) after createIndex on a referenced collection", function () {
        primeCache(runSpec);

        // A DDL on the foreign collection must invalidate the cached plan.
        assert.commandWorked(foreignColl.createIndex({e: 1}));

        assert.eq(
            runOnce(runSpec).miss,
            true,
            "run after createIndex on the foreign collection should miss the cache",
        );
        // And it should be cached again afterwards.
        assert.eq(runOnce(runSpec).hit, true, "run after re-caching should hit the cache");
    });

    it("re-caches (misses) after createIndex on the base collection", function () {
        primeCache(runSpec);

        assert.commandWorked(baseColl.createIndex({f: 1}));

        assert.eq(
            runOnce(runSpec).miss,
            true,
            "run after createIndex on the base collection should miss the cache",
        );
        assert.eq(runOnce(runSpec).hit, true, "run after re-caching should hit the cache");
    });

    // TODO(SERVER-130368): relevant-index invalidation will make this finer-grained,
    // DDLs on indexes unrelated to a cached plan should no longer invalidate it -- the tests
    // below that exercise unrelated indexes (dropIndex, collMod) will need to be updated to demonstrate:
    //  (1) dropping relevant index invalidates cache and
    //  (2) dropping irrelevant index does not invalidate cache.
    it("re-caches (misses) after dropIndex on an irrelevant index in a referenced collection", function () {
        // Create a throwaway index the join does not use, warm the cache, then drop it. Under the
        // current collection-level granularity this bumps the version and invalidates the plan even
        // though the plan never used this index (see the TODO note above).
        assert.commandWorked(foreignColl.createIndex({ddltmp: 1}));

        primeCache(runSpec);

        assert.commandWorked(foreignColl.dropIndex({ddltmp: 1}));

        assert.eq(
            runOnce(runSpec).miss,
            true,
            "run after dropIndex on the foreign collection should miss the cache",
        );
        assert.eq(runOnce(runSpec).hit, true, "run after re-caching should hit the cache");
    });

    // TODO(SERVER-130368): relevant-index invalidation will make this finer-grained,
    it("re-caches (misses) after collMod on an irrelevant index of a referenced collection", function () {
        // Use an auxiliary index the join does NOT use, so collMod refreshes an index entry (the
        // thing that bumps the collection version) without changing the query's plan or its
        // eligibility for the join plan cache. (collMod'ing a query-relevant index, e.g. hiding it,
        // would make the query uncacheable and defeat the point of this test.)
        assert.commandWorked(foreignColl.createIndex({aux: 1}));

        primeCache(runSpec);

        // collMod on an index routes through IndexCatalog::refreshEntry - ensure it bumps collectionVersion and invalidates cache.
        assert.commandWorked(
            foreignColl.runCommand("collMod", {
                index: {keyPattern: {aux: 1}, hidden: true},
            }),
        );

        assert.eq(
            runOnce(runSpec).miss,
            true,
            "run after collMod on the foreign collection's index should miss the cache",
        );
        assert.eq(runOnce(runSpec).hit, true, "run after re-caching should hit the cache");
    });

    it("re-caches (misses) after dropIndex on an index used by the cached plan", function () {
        // {a: 1} on the foreign collection provides an INLJ seek path on the join field 'a'. Force
        // INLJ while priming so the cached plan deterministically uses this index (rather than a
        // cost-based hash join on small data), and verify the plan shape. Reset the method before
        // the drop so the post-drop re-plan can fall back to a hash join and stay cacheable.
        assert.commandWorked(foreignColl.createIndex({a: 1}));

        setForcedJoinMethod("INLJ");
        primeCache(runSpec);
        assertPipelineUsesInlj();
        setForcedJoinMethod("any");

        // Dropping an index the cached plan uses must invalidate it. The foreign collection keeps
        // its other index, so the query stays eligible and the run is a miss, not a fallback.
        assert.commandWorked(foreignColl.dropIndex({a: 1}));

        assert.eq(
            runOnce(runSpec).miss,
            true,
            "run after dropIndex on an index used by the cached plan should miss the cache",
        );
        assert.eq(runOnce(runSpec).hit, true, "run after re-caching should hit the cache");
    });

    it("re-caches (misses) after collMod on an index used by the cached plan", function () {
        // {a: 1} on the foreign collection provides an INLJ seek path on the join field 'a'. Force
        // INLJ while priming so the cached plan deterministically uses this index, and verify the
        // plan shape. Reset the method before the collMod so the post-collMod re-plan can fall back
        // to a hash join and stay cacheable.
        assert.commandWorked(foreignColl.createIndex({a: 1}));

        setForcedJoinMethod("INLJ");
        primeCache(runSpec);
        assertPipelineUsesInlj();
        setForcedJoinMethod("any");

        // collMod-hiding an index the cached plan uses routes through refreshEntry and must
        // invalidate. The foreign collection keeps its other index, so the query stays eligible and
        // the run is a miss.
        assert.commandWorked(
            foreignColl.runCommand("collMod", {
                index: {keyPattern: {a: 1}, hidden: true},
            }),
        );

        assert.eq(
            runOnce(runSpec).miss,
            true,
            "run after collMod on an index used by the cached plan should miss the cache",
        );
        assert.eq(runOnce(runSpec).hit, true, "run after re-caching should hit the cache");
    });

    it("does not invalidate on a DDL to an unrelated collection", function () {
        primeCache(runSpec);

        // A DDL on a collection not referenced by the query must NOT invalidate the cached plan.
        const unrelatedColl = db["unrelated"];
        assert.commandWorked(unrelatedColl.insert({x: 1}));
        assert.commandWorked(unrelatedColl.createIndex({x: 1}));

        assert.eq(
            runOnce(runSpec).hit,
            true,
            "run after a DDL on an unrelated collection should still hit the cache",
        );

        unrelatedColl.drop();
    });

    // The following two tests document why renameCollection does NOT need to bump the collection
    // version: correctness after a rename is preserved by (1) the eligibility gate requiring the
    // referenced collection to exist, and (2) the per-collection UUID tag.
    it("renaming a referenced collection away makes the query ineligible (no cache use)", function () {
        primeCache(runSpec);

        // Rename the foreign collection away. The pipeline still references the old name, which no
        // longer exists, so the query is ineligible for join optimization and must not consult the
        // join plan cache at all (neither a hit nor a miss). $unwind drops the now-empty lookups.
        assert.commandWorked(foreignColl.renameCollection("rename_foreign_moved"));

        const deltas = runOnce({...runSpec, expectedResultCount: 0});
        assert.eq(deltas.hit, false, "ineligible query must not hit the join plan cache", deltas);
        assert.eq(deltas.miss, false, "ineligible query must not miss the join plan cache", deltas);

        db.getCollection("rename_foreign_moved").drop();
    });

    it("recreating a referenced collection at the old name invalidates via UUID mismatch", function () {
        primeCache(runSpec);

        // Move the foreign collection aside, then recreate a NEW collection at the same name with
        // the same data and indexes. The new collection has a different UUID, so the cached entry
        // (keyed on the same names) is found but its stored UUID no longer matches -> the run must
        // miss and re-optimize, even though nothing bumped the version counter.
        assert.commandWorked(foreignColl.renameCollection("rename_foreign_moved"));
        createForeignColl();

        assert.eq(
            runOnce(runSpec).miss,
            true,
            "recreated collection with a new UUID must invalidate the cached plan",
        );
        assert.eq(runOnce(runSpec).hit, true, "run after re-caching should hit the cache");

        db.getCollection("rename_foreign_moved").drop();
    });
});
