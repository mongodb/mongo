/**
 * Tests the executionStats counters returned in explain: 'nReturned', 'totalKeysExamined' and
 * 'totalDocsExamined'.
 *
 * @tags: [
 *   uses_explain,
 *   assumes_balancer_off,
 *   assumes_read_concern_local,
 *   requires_fcv_82,
 * ]
 */
import {before, describe, it} from "jstests/libs/mochalite.js";

describe("explain executionStats counters", function () {
    const coll = db[jsTestName()];

    function executionStats(cursor) {
        return cursor.explain("executionStats").executionStats;
    }

    describe("with a compound-indexed collection", function () {
        before(function () {
            coll.drop();
            assert.commandWorked(
                coll.createIndexes([
                    {a: 1, b: 1},
                    {b: 1, a: 1},
                ]),
            );
            assert.commandWorked(
                coll.insert([
                    {a: 0, b: 1},
                    {a: 1, b: 0},
                ]),
            );
        });

        it("reports counters for a range query", function () {
            const stats = executionStats(coll.find({a: {$gte: 0}, b: {$gte: 0}}));
            assert.eq(2, stats.nReturned, "wrong nReturned", {stats});
            assert.eq(2, stats.totalKeysExamined, "wrong totalKeysExamined", {stats});
            assert.eq(2, stats.totalDocsExamined, "wrong totalDocsExamined", {stats});
        });

        it("applies a negative limit", function () {
            const stats = executionStats(coll.find({a: {$gte: 0}, b: {$gte: 0}}).limit(-2));
            assert.eq(2, stats.nReturned, "wrong nReturned", {stats});
        });

        it("reports counters for a rooted $or query", function () {
            const stats = executionStats(
                coll.find({
                    $or: [
                        {a: {$gte: 0}, b: {$gte: 1}},
                        {a: {$gte: 1}, b: {$gte: 0}},
                    ],
                }),
            );
            assert.eq(2, stats.nReturned, "wrong nReturned", {stats});
        });
    });

    describe("with a regex predicate", function () {
        before(function () {
            coll.drop();
            assert.commandWorked(
                coll.createIndexes([
                    {a: 1, b: 1},
                    {b: 1, a: 1},
                ]),
            );
            assert.commandWorked(
                coll.insert([
                    {a: "0", b: "1"},
                    {a: "1", b: "0"},
                ]),
            );
        });

        it("examines more keys than it returns documents", function () {
            const stats = executionStats(coll.find({a: /0/, b: /1/}));
            assert.eq(1, stats.nReturned, "wrong nReturned", {stats});
            assert.eq(2, stats.totalKeysExamined, "wrong totalKeysExamined", {stats});
        });
    });

    describe("with an index-provided sort", function () {
        const numDocs = 300;

        before(function () {
            coll.drop();
            assert.commandWorked(coll.createIndexes([{a: 1}, {b: 1}]));
            const docs = [];
            for (let i = 0; i < numDocs; ++i) {
                docs.push({_id: i, a: i, b: i % 3});
            }
            assert.commandWorked(coll.insert(docs));
        });

        it("scans the whole hinted index while filtering on a non-indexed field", function () {
            const stats = executionStats(
                coll
                    .find({a: {$gte: 0}, b: 2})
                    .sort({a: 1})
                    .hint({a: 1}),
            );
            assert.eq(numDocs / 3, stats.nReturned, "wrong nReturned", {stats});
            assert.eq(numDocs, stats.totalKeysExamined, "wrong totalKeysExamined", {stats});
        });

        it("applies a limit to the number of documents returned", function () {
            const limit = 5;
            const stats = executionStats(
                coll
                    .find({a: {$gte: 0}, b: {$gte: 0}})
                    .sort({a: 1})
                    .hint({a: 1})
                    .limit(limit),
            );
            assert.eq(limit, stats.nReturned, "wrong nReturned", {stats});
        });
    });
});
