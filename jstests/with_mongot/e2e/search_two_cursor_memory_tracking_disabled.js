/**
 * Confirm that $search queries that use two cursors do not do per-operation memory tracking.
 *
 * @tags: [
 *   requires_sharding,
 *   assumes_unsharded_collection,
 *   requires_profiling,
 *   requires_getmore,
 *   queries_system_profile_collection,
 * ]
 */
import {getShardNames} from "jstests/libs/sharded_cluster_fixture_helpers.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {verifyProfilerMetrics} from "jstests/libs/query/memory_tracking_utils.js";
import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";

const kNumDocs = 200;
const kIndexName = "two_cursor_memtrack_index";
const kSearchCollName = jsTestName() + "_search";
// The outer source of the $unionWith case, with a separate namespace for labeling in profiler.
const kOuterCollName = jsTestName() + "_outer";

// A $search query with metadata, so that $$SEARCH_META resolves to something meaningful.
const kSearchQuery = {
    index: kIndexName,
    text: {query: "hello", path: "t"},
    count: {type: "total"},
};

// Include a memory tracked stage.
const kGroup = {$group: {_id: "$k", padding: {$max: "$padding"}}};

describe("$$SEARCH_META memory tracking on a sharded collection", function () {
    let testDB;
    let searchColl;
    let outerColl;
    let shardDBs;
    // Per-shard value of internalQueryMaxWriteToCurOpMemoryUsageBytes from before this test, so that
    // after() can put each shard back the way it found it.
    let prevMaxWriteToCurOpMemoryUsageBytes;

    before(function () {
        testDB = db.getSiblingDB(jsTestName());
        searchColl = testDB.getCollection(kSearchCollName);
        outerColl = testDB.getCollection(kOuterCollName);
        searchColl.drop();
        outerColl.drop();

        const shardNames = getShardNames(testDB.getMongo());
        assert.gte(shardNames.length, 2, "Test requires at least 2 shards");
        assert.commandWorked(testDB.adminCommand({enableSharding: testDB.getName(), primaryShard: shardNames[0]}));

        const docs = [];
        for (let i = 0; i < kNumDocs; i++) {
            docs.push({_id: i, k: i, t: "hello", padding: "x".repeat(100)});
        }

        // Populate 'coll' and put half of it on each shard, so both shards take part in every query.
        function populateAndShard(coll) {
            assert.commandWorked(coll.insert(docs));
            assert.commandWorked(testDB.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));
            assert.commandWorked(testDB.adminCommand({split: coll.getFullName(), middle: {_id: kNumDocs / 2}}));
            assert.commandWorked(
                testDB.adminCommand({
                    moveChunk: coll.getFullName(),
                    find: {_id: kNumDocs / 2},
                    to: shardNames[1],
                    _waitForDelete: true,
                }),
            );
        }
        populateAndShard(searchColl);
        populateAndShard(outerColl);

        createSearchIndex(searchColl, {name: kIndexName, definition: {mappings: {dynamic: true}}});

        const shardPrimaries = FixtureHelpers.getPrimaries(testDB);
        assert.gte(shardPrimaries.length, 2);
        shardDBs = shardPrimaries.map((conn) => conn.getDB(testDB.getName()));

        // Flush tracked memory to CurOp on every update, so that any tracking that happens at all
        // reaches the profiler instead of staying buffered below the reporting threshold.
        prevMaxWriteToCurOpMemoryUsageBytes = shardDBs.map(
            (shardDB) =>
                assert.commandWorked(
                    shardDB.adminCommand({
                        getParameter: 1,
                        internalQueryMaxWriteToCurOpMemoryUsageBytes: 1,
                    }),
                ).internalQueryMaxWriteToCurOpMemoryUsageBytes,
        );
        for (const shardDB of shardDBs) {
            assert.commandWorked(
                shardDB.adminCommand({
                    setParameter: 1,
                    internalQueryMaxWriteToCurOpMemoryUsageBytes: 1,
                }),
            );
        }
    });

    after(function () {
        shardDBs.forEach((shardDB, i) => {
            shardDB.setProfilingLevel(0);
            shardDB.system.profile.drop();
            assert.commandWorked(
                shardDB.adminCommand({
                    setParameter: 1,
                    internalQueryMaxWriteToCurOpMemoryUsageBytes: prevMaxWriteToCurOpMemoryUsageBytes[i],
                }),
            );
        });
        dropSearchIndex(searchColl, {name: kIndexName});
        searchColl.drop();
        outerColl.drop();
    });

    /**
     * Runs 'pipeline' on 'runOn' with the shards profiling everything, and returns the results plus
     * 'entriesFor(coll)': the profiler entries the shards recorded for that namespace, both the
     * aggregations mongos dispatched and the getMores that drained their cursors.
     */
    function runAndProfileShards({pipeline, runOn = searchColl}) {
        for (const shardDB of shardDBs) {
            shardDB.setProfilingLevel(0);
            shardDB.system.profile.drop();
            shardDB.setProfilingLevel(2);
        }

        const comment = jsTestName() + "_" + ObjectId().str;
        const results = runOn.aggregate(pipeline, {comment}).toArray();

        const entriesByShard = shardDBs.map((shardDB) => {
            shardDB.setProfilingLevel(0);
            return shardDB.system.profile
                .find({"command.comment": comment, "errCode": {$ne: ErrorCodes.StaleConfig}})
                .toArray();
        });

        // The verification helpers describe the operations of a single node, so entries stay grouped
        // by shard: one array of entries per shard, filtered to the namespace asked about.
        return {
            results,
            entriesFor: (coll) =>
                entriesByShard.map((entries) => entries.filter((entry) => entry.ns === coll.getFullName())),
        };
    }

    // The per-shard assertions below iterate 'entriesByShard', so an empty outer array would make
    // them pass without inspecting anything.
    function assertEveryShardProfiled(entriesByShard) {
        assert.gte(entriesByShard.length, 2, "expected profiler entries from both shards", {
            entriesByShard,
        });
    }

    /**
     * Reads whether the shard's search aggregation opened a metadata cursor, or null if 'entry' is
     * not the shard half of a search query.
     */
    function metaCursorFlagOf(entry) {
        const stage = entry.command.pipeline?.[0] ?? {};
        if (Object.hasOwn(stage, "$search")) {
            return stage.$search.requiresSearchMetaCursor;
        }
        return null;
    }

    function assertRequiresSearchMetaCursor(entriesByShard, expected) {
        assertEveryShardProfiled(entriesByShard);
        const searchEntries = entriesByShard.flat().filter((entry) => metaCursorFlagOf(entry) !== null);
        assert.gte(searchEntries.length, 2, "expected both shards to run the search aggregation", {
            entriesByShard,
        });
        for (const entry of searchEntries) {
            assert.eq(
                expected,
                metaCursorFlagOf(entry),
                "unexpected metadata-cursor flag on the shard's search stage",
                {entry},
            );
        }
    }

    /**
     * Asserts that no shard reported memory metrics, deferring to the shared verification helper so
     * that this reads as the exact negation of the reporting tests in noPassthrough/memory_tracking.
     */
    function assertNoMemoryMetrics(entriesByShard) {
        assertEveryShardProfiled(entriesByShard);
        for (const profilerEntries of entriesByShard) {
            // Absence of metrics only means something if there was something to look at. The control
            // case reports on a getMore, so require the getMores here too: entries consisting of the
            // aggregate alone would let the check pass while inspecting nothing.
            assert(
                profilerEntries.some((entry) => entry.op === "getmore"),
                "expected getMore entries among the profiled operations",
                {profilerEntries},
            );
            verifyProfilerMetrics({profilerEntries, verifyOptions: {expectMemoryMetrics: false}});
        }
    }

    /**
     * Asserts that every shard reported a non-zero peakTrackedMemBytes for the operations it ran on
     * this namespace.
     *
     * This checks only for the presence of a positive peak, rather than deferring to
     * verifyProfilerMetrics() as the negative case does, because a shard runs several *independent*
     * operations on the same namespace for one of these queries.
     */
    function assertReportsMemoryMetrics(entriesByShard) {
        assertEveryShardProfiled(entriesByShard);
        for (const profilerEntries of entriesByShard) {
            // What reports memory here does so during execution, so require the getMores: entries
            // consisting of the aggregate alone would have had nothing to report yet.
            assert(
                profilerEntries.some((entry) => entry.op === "getmore"),
                "expected getMore entries among the profiled operations",
                {profilerEntries},
            );
            assert(
                profilerEntries.some((entry) => entry.peakTrackedMemBytes > 0),
                "expected a positive peakTrackedMemBytes in at least one profiler entry",
                {profilerEntries},
            );
        }
    }

    // Every document of a $$SEARCH_META query carries the metadata of a search matching everything.
    function assertSearchMetaResolved(docs) {
        assert.eq(kNumDocs, docs.length, "expected one group per distinct 'k'");
        for (const doc of docs) {
            assert.eq(Number(doc.meta.count.total), kNumDocs, "unexpected $$SEARCH_META", {doc});
        }
    }

    it("does not report memory metrics for a $$SEARCH_META query", function () {
        const {results, entriesFor} = runAndProfileShards({
            pipeline: [{$search: kSearchQuery}, kGroup, {$addFields: {meta: "$$SEARCH_META"}}],
        });
        assertSearchMetaResolved(results);

        const entries = entriesFor(searchColl);
        assertRequiresSearchMetaCursor(entries, true);
        assertNoMemoryMetrics(entries);
    });

    it("does not report memory metrics for a $$SEARCH_META query inside $unionWith", function () {
        const {results, entriesFor} = runAndProfileShards({
            runOn: outerColl,
            pipeline: [
                kGroup,
                {
                    $unionWith: {
                        coll: kSearchCollName,
                        pipeline: [{$search: kSearchQuery}, kGroup, {$addFields: {meta: "$$SEARCH_META"}}],
                    },
                },
            ],
        });

        // One group per distinct 'k' from each collection; only the union's resolve $$SEARCH_META.
        assert.eq(2 * kNumDocs, results.length, "unexpected result count", {
            numResults: results.length,
        });
        assertSearchMetaResolved(results.filter((doc) => doc.hasOwnProperty("meta")));

        const unionEntries = entriesFor(searchColl);
        assertRequiresSearchMetaCursor(unionEntries, true);
        assertNoMemoryMetrics(unionEntries);

        // The outer namespace's operations are single-cursor and so not opted out. Note that the
        // merge among them runs the sub-pipeline's merging half, whose $group is tracked as usual:
        // the opt-out covers the operations owning the two cursors, not every operation of the query.
        assertReportsMemoryMetrics(entriesFor(outerColl));
    });

    it("does not report memory metrics for a $$SEARCH_META query inside $lookup", function () {
        // As for $unionWith, but reaching the sub-pipeline through the other stage that can carry a
        // $search of its own. The $limit keeps the number of sub-pipeline executions small; both
        // shards still run the outer $group that precedes it.
        const kNumLookups = 2;
        const {results, entriesFor} = runAndProfileShards({
            runOn: outerColl,
            pipeline: [
                kGroup,
                {$limit: kNumLookups},
                {
                    $lookup: {
                        from: kSearchCollName,
                        pipeline: [{$search: kSearchQuery}, kGroup, {$addFields: {meta: "$$SEARCH_META"}}],
                        as: "searchMeta",
                    },
                },
            ],
        });

        assert.eq(kNumLookups, results.length, "unexpected result count", {
            numResults: results.length,
        });
        for (const doc of results) {
            assertSearchMetaResolved(doc.searchMeta);
        }

        const lookupEntries = entriesFor(searchColl);
        assertRequiresSearchMetaCursor(lookupEntries, true);
        assertNoMemoryMetrics(lookupEntries);

        // The outer operation, on its own namespace, is single-cursor and so not opted out.
        assertReportsMemoryMetrics(entriesFor(outerColl));
    });
});
