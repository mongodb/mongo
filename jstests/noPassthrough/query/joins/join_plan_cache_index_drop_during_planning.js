/**
 * Tests concurrent index drop during join query planning.
 *
 * Join query planning samples each collection which may yield. This test forces the sampling
 * scan to yield after every document and uses a failpoint to pause the query inside the yield,
 * locks released, while a dropIndexes commits. The query then restores onto the new catalog
 * version but keeps planning from index entries extracted before the yield, so it would cache a
 * plan referencing the dropped index under post-drop collection tags. A subsequent cache hit should
 * not trigger tassert 12926305 in fromCachedJoinPlan which asserts the index refereneced by an INLJ node of the cached plan still exists.
 *
 * This is a regression test for SERVER-134800.
 *
 * @tags: [
 *   requires_fcv_91,
 *   requires_sbe,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";

const kForeignDocs = 20000;

describe("join plan cache entries whose planning spanned a concurrent index drop", function () {
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

        const docs = [];
        for (let i = 0; i < kForeignDocs; i++) {
            docs.push({a: i, pad: "x".repeat(32)});
        }

        // Both sides of the join predicate need an index for path arrayness information.
        assert.commandWorked(this.db.base.createIndex({a: 1}));
        assert.commandWorked(this.db.foreign.createIndex({a: 1}));

        assert.commandWorked(this.db.foreign.insertMany(docs));

        assert.commandWorked(this.db.base.insertMany([{a: 1}, {a: 2}, {a: 3}]));

        // Keeps path 'a' known non-array after a_1 is dropped, so the yield-restore arrayness
        // check does not kill the query mid-planning.
        assert.commandWorked(this.db.foreign.createIndex({dummy: 1, a: 1}));

        this.pipeline = [
            {$match: {a: {$gt: 0}}},
            {$lookup: {from: "foreign", localField: "a", foreignField: "a", as: "f"}},
            {$unwind: "$f"},
        ];
    });

    after(function () {
        MongoRunner.stopMongod(this.conn);
    });

    it("invalidates a cached plan built across a mid-planning index drop", function () {
        // Hang inside the sampling yield, after locks have been released.
        const fp = configureFailPoint(this.conn, "setYieldAllLocksHang");
        const awaitQuery = startParallelShell(
            funWithArgs(
                function (pipeline, dbName) {
                    // The query may die on restore (QueryPlanKilled) once the index is gone; the
                    // cache entry is written before that happens.
                    const res = db
                        .getSiblingDB(dbName)
                        .runCommand({aggregate: "base", pipeline: pipeline, cursor: {}});
                    assert.commandWorkedOrFailedWithCode(res, ErrorCodes.QueryPlanKilled);
                },
                this.pipeline,
                this.db.getName(),
            ),
            this.conn.port,
        );

        fp.wait();
        // Drop index while the query is yielded.
        assert.commandWorked(this.db.foreign.dropIndex("a_1"));
        fp.off();
        awaitQuery();

        const statsBefore = this.db.serverStatus().metrics.query.planCache.join;

        // The cached entry is tagged with the pre-yield catalog version, so this lookup fails
        // fingerprint revalidation, invalidates the entry, and replans.
        assert.commandWorked(
            this.db.runCommand({aggregate: "base", pipeline: this.pipeline, cursor: {}}),
        );

        const statsAfter = this.db.serverStatus().metrics.query.planCache.join;
        assert.eq(statsAfter.invalidations - statsBefore.invalidations, 1, {
            statsBefore,
            statsAfter,
        });
        assert.eq(statsAfter.misses - statsBefore.misses, 1, {statsBefore, statsAfter});
        assert.eq(statsAfter.hits - statsBefore.hits, 0, {statsBefore, statsAfter});
    });
});
