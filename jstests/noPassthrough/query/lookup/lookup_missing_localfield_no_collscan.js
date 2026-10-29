/**
 * Tests that $lookup inner queries keep using the foreign collection's _id index for documents
 * whose localField is missing or null, even after an earlier input document ran an _id point
 * query. DocumentSourceLookUp reuses one ExpressionContext for all per-document inner queries,
 * and the isIdHackQuery flag set by the first point query must not leak into the planning of
 * the {_id: {$eq: null}} queries that follow: a stale flag makes QueryPlannerParams skip index
 * discovery, so every null-key lookup collection scans the entire foreign collection.
 *
 * This serves as a regression test for SERVER-133279. We only run this test in variants with
 * SBE disabled because it relies on classic's $lookup behavior of reusing the same
 * ExpressionContext for all inner queries, so SBE cannot have this bug by design.
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";

describe("$lookup on _id with missing localField values", function () {
    let conn;
    let db;

    const pipeline = (sortDir) => [
        {$match: {_id: {$in: ["host_0", "host_1"]}}},
        // The sort direction controls whether the point-query lookup (host_0) or the
        // null-key lookup (host_1) runs first.
        {$sort: {_id: sortDir}},
        {
            $lookup: {
                from: "tasks",
                localField: "running_task",
                foreignField: "_id",
                as: "task_full",
            },
        },
        {$unwind: {path: "$task_full", preserveNullAndEmptyArrays: true}},
        {$count: "count"},
    ];

    function getLookupExecStats(sortDir) {
        const explain = db.hosts.explain("executionStats").aggregate(pipeline(sortDir));
        const lookupStage = explain.stages.find((stage) => stage.hasOwnProperty("$lookup"));
        assert(lookupStage, "expected a $lookup stage in the explain output", {explain});
        return lookupStage;
    }

    function assertUsesForeignIdIndex(sortDir) {
        const stats = getLookupExecStats(sortDir);
        assert.eq(stats.collectionScans, 0, "expected no foreign collection scans", {stats});
        // A constant number of fetches for the point query; the null-key query matches nothing.
        // A collection scan would examine every foreign document per null-key input document.
        assert.lte(stats.totalDocsExamined, 2, "expected a constant number of document fetches", {
            stats,
        });
        assert.contains("_id_", stats.indexesUsed, "expected the foreign _id index to be used", {
            stats,
        });
    }

    before(function () {
        conn = MongoRunner.runMongod({});
        db = conn.getDB(jsTestName());

        // Skip the test if SBE is fully enabled, it does not suffer from the bug this test is meant to catch.
        const isSbeEnabled = checkSbeFullyEnabled(db);
        if (isSbeEnabled) {
            jsTest.log.info("Skipping test because SBE is enabled");
            MongoRunner.stopMongod(conn);
            quit();
        }

        const tasks = [];
        for (let i = 0; i < 20; i++) {
            tasks.push({_id: "task_" + i});
        }
        assert.commandWorked(db.tasks.insertMany(tasks));

        // host_0's inner lookup query is a simple _id point query; host_1 has no
        // running_task, so its inner query is {_id: {$eq: null}}.
        assert.commandWorked(
            db.hosts.insertMany([{_id: "host_0", running_task: "task_0"}, {_id: "host_1"}]),
        );
    });

    after(function () {
        MongoRunner.stopMongod(conn);
    });

    it("uses the _id index when the null-key lookup runs before the point query", function () {
        assertUsesForeignIdIndex(-1);
    });

    it("uses the _id index when the null-key lookup runs after the point query", function () {
        assertUsesForeignIdIndex(1);
    });
});
