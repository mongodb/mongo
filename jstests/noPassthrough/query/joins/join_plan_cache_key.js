/**
 * Verifies that the join plan cache key hash is exposed in explain output as
 * queryPlanner.joinPlanCacheKey, and that the hash changes when the cache key
 * encoding should change and stays stable when it should not.
 *
 * @tags: [
 *   requires_fcv_90,
 *   requires_sbe,
 * ]
 */

import {getQueryPlanner} from "jstests/libs/query/analyze_plan.js";
import {joinOptUsed} from "jstests/libs/query/join_utils.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";

describe("join plan cache key in explain", function () {
    before(function () {
        this.conn = MongoRunner.runMongod({
            setParameter: {
                internalEnableJoinOptimization: true,
            },
        });

        const db = this.conn.getDB(jsTestName());
        this.baseColl = db[jsTestName()];
        this.foreignColl = db[jsTestName() + "_foreign"];

        assert.commandWorked(
            this.baseColl.insertMany([
                {a: 1, b: 1, d: 1},
                {a: 1, b: 2, d: 2},
                {a: 2, b: 1, d: 1},
                {a: 2, b: 2, d: 2},
            ]),
        );
        assert.commandWorked(
            this.foreignColl.insertMany([
                {a: 1, b: 1, c: "foo", d: 1},
                {a: 1, b: 2, c: "bar", d: 2},
                {a: 2, b: 1, c: "baz", d: 1},
                {a: 2, b: 2, c: "qux", d: 2},
            ]),
        );
        assert.commandWorked(this.baseColl.createIndex({a: 1, b: 1}));
        assert.commandWorked(this.foreignColl.createIndex({a: 1, b: 1}));

        // Returns the join plan cache key hash from the queryPlanner section of the explain for
        // 'pipeline', asserting join optimization actually ran.
        this.getKey = (pipeline) => {
            const explain = this.baseColl.explain().aggregate(pipeline);
            assert(joinOptUsed(explain), "expected join optimization to be used", {explain});
            const planner = getQueryPlanner(explain);
            assert(
                planner.hasOwnProperty("joinPlanCacheKey"),
                "expected joinPlanCacheKey in queryPlanner",
                {planner},
            );
            return planner.joinPlanCacheKey;
        };

        // A cache-eligible $lookup/$unwind join with a single-table predicate on the base
        // collection and an equality join on field 'a'.
        this.makePipeline = ({matchValue = 0, foreignField = "a"} = {}) => [
            {$match: {a: {$gt: matchValue}}},
            {
                $lookup: {
                    from: this.foreignColl.getName(),
                    localField: "a",
                    foreignField: foreignField,
                    as: "foreignColl",
                },
            },
            {$unwind: "$foreignColl"},
        ];
    });

    after(function () {
        MongoRunner.stopMongod(this.conn);
    });

    it("exposes a joinPlanCacheKey hash when join optimization is used", function () {
        const key = this.getKey(this.makePipeline());
        assert.eq(typeof key, "string", "joinPlanCacheKey should be a hex string");
        assert.gt(key.length, 0, "joinPlanCacheKey should be non-empty");
    });

    it("produces the same hash for structurally identical queries", function () {
        assert.eq(this.getKey(this.makePipeline()), this.getKey(this.makePipeline()));
    });

    it("produces the same hash when only predicate constants differ", function () {
        assert.eq(
            this.getKey(this.makePipeline({matchValue: 0})),
            this.getKey(this.makePipeline({matchValue: 1})),
        );
    });

    it("produces a different hash when the join structure differs", function () {
        assert.neq(
            this.getKey(this.makePipeline({foreignField: "a"})),
            this.getKey(this.makePipeline({foreignField: "b"})),
        );
    });
});
