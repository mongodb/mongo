/**
 * Regression test for a bug where the classic engine's DISTINCT_SCAN stage would return a
 * premature IS_EOF when its embedded fetch operation yielded on a WriteConflictException (WCE).
 * All remaining distinct groups were silently dropped.
 *
 * The bug only manifests when the projected output cannot be served by the index alone,
 * causing the planner to push a FETCH into the DISTINCT_SCAN.
 *
 * @tags: [
 *   requires_wiredtiger,
 *   does_not_support_stepdowns,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";

describe("DISTINCT_SCAN fetch-yield retry", function () {
    let conn;
    let db;

    before(function () {
        conn = MongoRunner.runMongod({});
        assert.neq(null, conn, "mongod failed to start");
        db = conn.getDB(jsTestName());
        assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "forceClassicEngine"}));
    });

    after(function () {
        MongoRunner.stopMongod(conn);
    });

    function aggregateWithRetry(coll, pipeline) {
        while (true) {
            const res = coll.getDB().runCommand({aggregate: coll.getName(), pipeline, cursor: {}});
            if (res.ok) {
                return res.cursor.firstBatch;
            }
            assert.eq(112, res.code, {msg: "Unexpected error from aggregate", res});
        }
    }

    it("DISTINCT_SCAN with fetch returns all distinct groups when fetch yields on WCE", function () {
        // $top projects 'c' which is not in the {a:1,b:1} index, forcing a fetch.
        const coll = db[jsTestName() + "_no_shard_filter"];
        coll.drop();
        assert.commandWorked(coll.createIndex({a: 1, b: 1}));

        const docs = [];
        for (let a = 0; a < 10; ++a) {
            docs.push({a: a, b: 0, c: a});
            docs.push({a: a, b: 1, c: a});
        }
        assert.commandWorked(coll.insertMany(docs));

        const pipeline = [{$group: {_id: "$a", first: {$top: {sortBy: {a: 1, b: 1}, output: "$c"}}}}];

        assert.commandWorked(
            db.adminCommand({
                configureFailPoint: "WTWriteConflictExceptionForReads",
                mode: {activationProbability: 0.05},
            }),
        );

        try {
            for (let i = 0; i < 50; ++i) {
                const results = aggregateWithRetry(coll, pipeline);
                assert.eq(10, results.length, {
                    iteration: i,
                    msg: "Expected 10 distinct groups; some were silently dropped",
                    results,
                });

                const byId = {};
                for (const r of results) {
                    byId[r._id] = r.first;
                }
                for (let a = 0; a < 10; ++a) {
                    assert(byId.hasOwnProperty(a), {iteration: i, msg: `Missing group a=${a}`, byId});
                    assert.eq(a, byId[a], {iteration: i, msg: `Wrong $top output for a=${a}`, byId});
                }
            }
        } finally {
            assert.commandWorked(
                db.adminCommand({configureFailPoint: "WTWriteConflictExceptionForReads", mode: "off"}),
            );
        }
    });

    it("DISTINCT_SCAN with fetch returns all distinct groups ($first accumulator) when fetch yields on WCE", function () {
        // $first projects 'extra' which is not in the {a:1} index, forcing a fetch.
        const coll = db[jsTestName() + "_first_acc"];
        coll.drop();
        assert.commandWorked(coll.createIndex({a: 1}));

        const docs = [];
        for (let a = 0; a < 10; ++a) {
            docs.push({a: a, extra: a * 2});
        }
        assert.commandWorked(coll.insertMany(docs));

        const pipeline = [{$group: {_id: "$a", val: {$first: "$extra"}}}];

        assert.commandWorked(
            db.adminCommand({
                configureFailPoint: "WTWriteConflictExceptionForReads",
                mode: {activationProbability: 0.05},
            }),
        );

        try {
            for (let i = 0; i < 50; ++i) {
                const results = aggregateWithRetry(coll, pipeline);
                assert.eq(10, results.length, {
                    iteration: i,
                    msg: "Expected 10 distinct groups; some were silently dropped",
                    results,
                });

                const byId = {};
                for (const r of results) {
                    byId[r._id] = r.val;
                }
                for (let a = 0; a < 10; ++a) {
                    assert(byId.hasOwnProperty(a), {iteration: i, msg: `Missing group a=${a}`, byId});
                    assert.eq(a * 2, byId[a], {iteration: i, msg: `Wrong $first value for a=${a}`, byId});
                }
            }
        } finally {
            assert.commandWorked(
                db.adminCommand({configureFailPoint: "WTWriteConflictExceptionForReads", mode: "off"}),
            );
        }
    });
});
