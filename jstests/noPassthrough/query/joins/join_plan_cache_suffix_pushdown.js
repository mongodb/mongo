/**
 * Tests that a join plan recovered from the join plan cache still pushes the SBE-eligible pipeline
 * suffix down to SBE, just like the plan produced on a cache miss.
 *
 * Explain never consults the join plan cache, so the pushdown on the cache-hit path is observed
 * through the 'numSuffixSourcesPushedToSbe' / 'numResidualClassicSources' serverStatus counters.
 *
 * @tags: [
 *   requires_fcv_91,
 *   requires_sbe,
 * ]
 */

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {afterEach, beforeEach, describe, it} from "jstests/libs/mochalite.js";

describe("join plan cache suffix pushdown", function () {
    function joinOptMetrics(db) {
        const section = db.getSiblingDB("admin").serverStatus().metrics.query.joinOptimization;
        assert(section, "missing joinOptimization serverStatus section");
        return section;
    }

    function planCacheStats(db) {
        const join = db.getSiblingDB("admin").serverStatus().metrics.query.planCache.join;
        assert(join, "missing metrics.query.planCache.join section");
        return join;
    }

    /**
     * Runs 'pipeline' and returns the suffix lowering counters attributable to that single run,
     * along with whether the run was a join plan cache hit.
     */
    function runAndMeasure(coll, pipeline) {
        const db = coll.getDB();
        const beforeMetrics = joinOptMetrics(db);
        const beforeCache = planCacheStats(db);

        const results = coll.aggregate(pipeline).toArray();

        const afterMetrics = joinOptMetrics(db);
        const afterCache = planCacheStats(db);
        return {
            results,
            pushedToSbe:
                afterMetrics.numSuffixSourcesPushedToSbe -
                beforeMetrics.numSuffixSourcesPushedToSbe,
            residualClassic:
                afterMetrics.numResidualClassicSources - beforeMetrics.numResidualClassicSources,
            cacheHit: afterCache.hits - beforeCache.hits === 1,
            cacheMiss: afterCache.misses - beforeCache.misses === 1,
        };
    }

    beforeEach(function () {
        this.conn = MongoRunner.runMongod({
            setParameter: {
                internalEnableJoinOptimization: true,
                internalEnableJoinPlanCache: true,
            },
        });

        const db = this.conn.getDB(jsTestName());
        this.db = db;
        this.base = db[jsTestName() + "_base"];
        this.foreign = db[jsTestName() + "_a"];

        assert.commandWorked(
            this.base.insertMany([
                {_id: 1, a: 1, b: 10},
                {_id: 2, a: 1, b: 20},
                {_id: 3, a: 2, b: 30},
            ]),
        );
        assert.commandWorked(
            this.foreign.insertMany([
                {_id: 101, a: 1, tag: "left"},
                {_id: 102, a: 2, tag: "right"},
            ]),
        );

        // Join indexes.
        assert.commandWorked(this.base.createIndex({a: 1}));
        assert.commandWorked(this.foreign.createIndex({a: 1}));
        // Path-arrayness metadata indexes.
        assert.commandWorked(this.base.createIndex({dummy: 1, a: 1, b: 1}));
        assert.commandWorked(this.foreign.createIndex({dummy: 1, a: 1, tag: 1}));

        this.joinPrefix = [
            {$match: {a: {$gt: 0}}},
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
    });

    afterEach(function () {
        MongoRunner.stopMongod(this.conn);
    });

    it("pushes down a fully SBE-eligible suffix on a cache hit", function () {
        const pipeline = [...this.joinPrefix, {$group: {_id: "$foreign.tag", total: {$sum: "$b"}}}];

        const miss = runAndMeasure(this.base, pipeline);
        assert(miss.cacheMiss, "expected the first run to be a join plan cache miss");
        assert.gt(miss.pushedToSbe, 0, "expected the suffix to be pushed to SBE on a cache miss");

        const hit = runAndMeasure(this.base, pipeline);
        assert(hit.cacheHit, "expected the second run to be a join plan cache hit");
        assert.eq(
            hit.pushedToSbe,
            miss.pushedToSbe,
            "cache hit pushed a different number of suffix stages to SBE than the cache miss",
        );
        assert.eq(
            hit.residualClassic,
            miss.residualClassic,
            "cache hit left a different number of suffix stages in classic than the cache miss",
        );
        assertArrayEq({actual: hit.results, expected: miss.results});
    });

    it("pushes down a partially SBE-eligible suffix on a cache hit", function () {
        // '$_internalInhibitOptimization' is not SBE-eligible, so it and everything after it must
        // remain in classic while the leading '$group' is pushed down.
        const pipeline = [
            ...this.joinPrefix,
            {$group: {_id: "$foreign.tag", total: {$sum: "$b"}}},
            {$_internalInhibitOptimization: {}},
            {$sort: {_id: 1}},
        ];

        const miss = runAndMeasure(this.base, pipeline);
        assert(miss.cacheMiss, "expected the first run to be a join plan cache miss");
        assert.gt(miss.pushedToSbe, 0, "expected part of the suffix to be pushed to SBE");
        assert.gt(miss.residualClassic, 0, "expected part of the suffix to remain in classic");

        const hit = runAndMeasure(this.base, pipeline);
        assert(hit.cacheHit, "expected the second run to be a join plan cache hit");
        assert.eq(hit.pushedToSbe, miss.pushedToSbe, "cache hit pushed a different suffix prefix");
        assert.eq(hit.residualClassic, miss.residualClassic, "cache hit left a different residual");
        // Guards against a pushed-down stage running twice (or not at all) on the cache-hit path.
        assert.eq(hit.results, miss.results);
    });
});
