/**
 * Tests concurrent index DDL during join query planning.
 *
 * Join query planning samples each collection which may yield. This test forces the sampling
 * query to yield after every document and uses a failpoint to pause the query during the yield
 * while a dropIndexes or createIndexes commits.
 *
 * The tests in this file validate that the join plan cache is able to detect and handle the
 * cases in which the state of the catalog differs pre and post sampling query yield due to a concurrent DDL.
 *
 * @tags: [
 *   requires_fcv_91,
 *   requires_sbe,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {joinPlanCacheStatsDelta} from "jstests/libs/query/join_utils.js";
import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";

const kForeignDocs = 20000;

describe("join plan cache entries whose planning spanned a concurrent index DDL", function () {
    before(function () {
        this.conn = MongoRunner.runMongod({
            setParameter: {
                internalEnableJoinOptimization: true,
                internalEnableJoinPlanCache: true,
                // Force the join method so the winning plan always uses the dropped index.
                internalJoinMethod: "INLJ",
                internalQueryExecYieldIterations: 1,
                internalQueryExecYieldPeriodMS: 1,
            },
        });
        this.db = this.conn.getDB(jsTestName());
    });

    after(function () {
        MongoRunner.stopMongod(this.conn);
    });

    function resetCollections(db) {
        const docs = [];
        for (let i = 0; i < kForeignDocs; i++) {
            docs.push({a: i, pad: "x".repeat(32)});
        }

        db.base.drop();
        db.foreign.drop();

        // Both sides of the join predicate need an index for path arrayness information.
        assert.commandWorked(db.base.createIndex({a: 1}));
        assert.commandWorked(db.foreign.createIndex({a: 1}));

        assert.commandWorked(db.foreign.insertMany(docs));
        assert.commandWorked(db.base.insertMany([{a: 1}, {a: 2}, {a: 3}]));

        // Keeps path 'a' known non-array after a_1 is dropped, so the yield-restore arrayness
        // check does not kill the query mid-planning.
        assert.commandWorked(db.foreign.createIndex({dummy: 1, a: 1}));
    }

    beforeEach(function () {
        resetCollections(this.db);
        assert.commandWorked(
            this.db.adminCommand({
                setParameter: 1,
                internalQueryExecYieldIterations: 1,
                internalQueryExecYieldPeriodMS: 1,
            }),
        );
    });

    // Concurrent dropIndex case: Test that the query runs successfully but avoids being cached if
    // an index that is relevant to the query is concurrently dropped and a sampling yield takes place.
    // Regression test for SERVER-134800.
    it("does not cache a plan built across a mid-planning index drop", function () {
        const pipeline = [
            {$match: {a: {$gt: 0}}},
            {$lookup: {from: "foreign", localField: "a", foreignField: "a", as: "f"}},
            {$unwind: "$f"},
        ];

        // Hang inside the sampling yield, after locks have been released.
        const fp = configureFailPoint(this.conn, "setYieldAllLocksHang");
        const awaitQuery = startParallelShell(
            funWithArgs(
                function (pipeline, dbName) {
                    // The query may die on restore (QueryPlanKilled) once the index is gone.
                    const res = db
                        .getSiblingDB(dbName)
                        .runCommand({aggregate: "base", pipeline: pipeline, cursor: {}});
                    assert.commandWorkedOrFailedWithCode(res, ErrorCodes.QueryPlanKilled);
                },
                pipeline,
                this.db.getName(),
            ),
            this.conn.port,
        );

        fp.wait();
        // Drop index while the query is yielded.
        assert.commandWorked(this.db.foreign.dropIndex("a_1"));
        fp.off();
        awaitQuery();

        // Reset yield knobs so that the following queries are not subject to the yield overhead.
        assert.commandWorked(
            this.db.adminCommand({
                setParameter: 1,
                internalQueryExecYieldIterations: -1,
                internalQueryExecYieldPeriodMS: 10,
            }),
        );

        // The first query's planning spanned the dropIndex commit, so its plan was not cached;
        // this run misses and replans, and the following run is served from the cache.
        let delta = joinPlanCacheStatsDelta(this.db, () => {
            assert.commandWorked(
                this.db.runCommand({aggregate: "base", pipeline: pipeline, cursor: {}}),
            );
        });
        assert.eq(delta, {hitDelta: 0, missDelta: 1, invalidationDelta: 0}, {delta});

        delta = joinPlanCacheStatsDelta(this.db, () => {
            assert.commandWorked(
                this.db.runCommand({aggregate: "base", pipeline: pipeline, cursor: {}}),
            );
        });
        assert.eq(delta, {hitDelta: 1, missDelta: 0, invalidationDelta: 0}, {delta});
    });

    // Concurrent createIndex case: Test that the query runs successfully but avoids being cached if
    // an index that is relevant to the query is concurrently created and a sampling yield takes place.
    // Regression test for SERVER-135049.
    it("does not cache a plan built across a mid-planning index create", function () {
        // Add docs with field "b" so that the planner chooses the obvious (soon-to-be-created) index on "b" for the single table access.
        const docs = [];
        for (let i = 0; i < 10000; i++) {
            docs.push({a: i, b: i});
        }
        assert.commandWorked(this.db.base.insertMany(docs));

        const pipeline = [
            {$match: {b: 9999}},
            {$lookup: {from: "foreign", localField: "a", foreignField: "a", as: "f"}},
            {$unwind: "$f"},
        ];

        const fp = configureFailPoint(this.conn, "setYieldAllLocksHang");
        const awaitQuery = startParallelShell(
            funWithArgs(
                function (pipeline, dbName) {
                    const res = db
                        .getSiblingDB(dbName)
                        .runCommand({aggregate: "base", pipeline: pipeline, cursor: {}});
                    assert.commandWorked(res);
                    assert.eq(res.cursor.firstBatch.length, 1, {res});
                },
                pipeline,
                this.db.getName(),
            ),
            this.conn.port,
        );

        fp.wait();
        try {
            // An index build's own collection scan would yield into the armed failpoint.
            assert.commandWorked(
                this.db.adminCommand({
                    setParameter: 1,
                    internalQueryExecYieldIterations: -1,
                    internalQueryExecYieldPeriodMS: 10000,
                }),
            );
            assert.commandWorked(this.db.base.createIndex({b: 1}));
        } finally {
            fp.off();
            // Run the rest of the test at the default yield cadence.
            assert.commandWorked(
                this.db.adminCommand({
                    setParameter: 1,
                    internalQueryExecYieldIterations: -1,
                    internalQueryExecYieldPeriodMS: 10,
                }),
            );
        }
        awaitQuery();

        // Reset yield knobs so that the following queries are not subject to the yield overhead.
        assert.commandWorked(
            this.db.adminCommand({
                setParameter: 1,
                internalQueryExecYieldIterations: -1,
                internalQueryExecYieldPeriodMS: 10,
            }),
        );

        assert.commandWorked(this.db.base.dropIndex("b_1"));

        // The first query's planning spanned the createIndex commit, so its plan was not cached;
        // this run misses and replans, and the following run is served from the cache.
        let delta = joinPlanCacheStatsDelta(this.db, () => {
            assert.commandWorked(
                this.db.runCommand({aggregate: "base", pipeline: pipeline, cursor: {}}),
            );
        });
        assert.eq(delta, {hitDelta: 0, missDelta: 1, invalidationDelta: 0}, {delta});

        delta = joinPlanCacheStatsDelta(this.db, () => {
            assert.commandWorked(
                this.db.runCommand({aggregate: "base", pipeline: pipeline, cursor: {}}),
            );
        });
        assert.eq(delta, {hitDelta: 1, missDelta: 0, invalidationDelta: 0}, {delta});
    });
});
