/**
 * Tests that when the join optimizer produces a hash join (SBE HashJoinStage) and the query
 * disallows disk use, the query errors out with QueryExceededMemoryLimitNoDiskUseAllowed instead of
 * silently spilling to disk. Also verifies that the same query succeeds (spilling) when disk use is
 * allowed.
 *
 * @tags: [
 *   requires_fcv_90,
 *   requires_sbe,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {assertAllJoinsUseMethod} from "jstests/libs/query/join_utils.js";

describe("$lookup hash join honors allowDiskUse", function () {
    let conn;
    let localColl;
    const pipeline = [
        {$lookup: {from: "foreign", as: "joined", localField: "a", foreignField: "b"}},
        {$unwind: "$joined"},
    ];

    before(function () {
        conn = MongoRunner.runMongod({setParameter: {featureFlagPathArrayness: true}});
        const db = conn.getDB(jsTestName());
        localColl = db.local;
        const foreignColl = db.foreign;

        assert.commandWorked(
            localColl.insertMany([
                {_id: 0, a: 1},
                {_id: 1, a: 2},
                {_id: 2, a: 3},
            ]),
        );
        assert.commandWorked(
            foreignColl.insertMany([
                {_id: 0, b: 1, payload: "x".repeat(64)},
                {_id: 1, b: 2, payload: "y".repeat(64)},
                {_id: 2, b: 3, payload: "z".repeat(64)},
            ]),
        );
        // Indexes provide multikeyness info for path arrayness, matching other join tests.
        assert.commandWorked(localColl.createIndex({dummy: 1, a: 1}));
        assert.commandWorked(foreignColl.createIndex({dummy: 1, b: 1}));

        // Force the join optimizer to use a hash join, and set a tiny spill threshold so that
        // building the hash table over the foreign side must spill.
        assert.commandWorked(
            db.adminCommand({
                setParameter: 1,
                internalEnableJoinOptimization: true,
                internalJoinMethod: "HJ",
                internalQuerySlotBasedExecutionHashJoinApproxMemoryUseInBytesBeforeSpill: 1,
            }),
        );
    });

    after(function () {
        MongoRunner.stopMongod(conn);
    });

    it("plans a hash join for the pipeline", function () {
        assertAllJoinsUseMethod(localColl.explain().aggregate(pipeline), "HJ");
    });

    it("errors instead of spilling when allowDiskUse is false", function () {
        assert.throwsWithCode(
            () => localColl.aggregate(pipeline, {allowDiskUse: false}).toArray(),
            ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed,
        );
    });

    it("spills and succeeds when allowDiskUse is true", function () {
        const results = localColl.aggregate(pipeline, {allowDiskUse: true}).toArray();
        assert.eq(3, results.length, results);
    });
});
