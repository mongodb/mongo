/**
 * Verifies that the join plan cache key hash is exposed in explain output as
 * queryPlanner.joinOptimizer.planCacheKey, and that the hash changes when the cache key
 * encoding should change and stays stable when it should not.
 *
 * See jstests/query_golden/join_opt/join_plan_cache_key_md.js for additional test cases.
 *
 * @tags: [
 *   requires_fcv_90,
 *   requires_sbe,
 * ]
 */

import {getQueryPlanner} from "jstests/libs/query/analyze_plan.js";
import {joinOptUsed} from "jstests/libs/query/join_utils.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {QuerySettingsUtils} from "jstests/libs/query/query_settings_utils.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

// Builds per-query settings that hint the {a: 1, b: 1} index on 'coll'.
function makeSettings(coll) {
    return {
        indexHints: {
            ns: {db: coll.getDB().getName(), coll: coll.getName()},
            allowedIndexes: [{a: 1, b: 1}],
        },
    };
}

describe("join plan cache key in explain on a standalone mongod", function () {
    before(function () {
        this.conn = MongoRunner.runMongod({
            setParameter: {
                internalEnableJoinOptimization: true,
            },
        });

        const db = this.conn.getDB(jsTestName());
        this.baseColl = db[jsTestName()];
        this.foreignColl = db[jsTestName() + "_foreign"];

        this.settings = makeSettings(this.baseColl);

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
        // 'pipeline', asserting join optimization actually ran. If 'querySettings' is given, it is
        // passed to the command as per-query settings.
        this.getKey = (pipeline, querySettings = {}) => {
            const explain = this.baseColl.explain().aggregate(pipeline, {querySettings});
            assert(joinOptUsed(explain), "expected join optimization to be used", {explain});
            const planner = getQueryPlanner(explain);
            assert(
                planner.joinOptimizer?.hasOwnProperty("planCacheKey"),
                "expected planCacheKey in queryPlanner.joinOptimizer",
                {planner},
            );
            return planner.joinOptimizer.planCacheKey;
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

    it("exposes a planCacheKey hash when join optimization is used", function () {
        const key = this.getKey(this.makePipeline());
        assert.eq(typeof key, "string", "planCacheKey should be a hex string");
        assert.gt(key.length, 0, "planCacheKey should be non-empty");
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

    it("produces a different hash when index hints are set", function () {
        const pipeline = this.makePipeline();
        assert.neq(this.getKey(pipeline), this.getKey(pipeline, this.settings));
    });

    it("produces a different hash when queryFramework is set", function () {
        const pipeline = this.makePipeline();
        assert.neq(
            this.getKey(pipeline, {...this.settings, queryFramework: "sbe"}),
            this.getKey(pipeline, this.settings),
        );
    });

    it("ignores comment when computing the hash", function () {
        const pipeline = this.makePipeline();
        assert.eq(
            this.getKey(pipeline, this.settings),
            this.getKey(pipeline, {...this.settings, comment: "a comment"}),
        );
    });
});

// Cluster-wide query settings (setQuerySettings) are stored as a cluster parameter, so they can only
// be set on replica sets or sharded clusters.
describe("join plan cache key in explain on a replica set with cluster-wide query settings", function () {
    before(function () {
        this.rst = new ReplSetTest({
            nodes: 1,
            nodeOptions: {setParameter: {internalEnableJoinOptimization: true}},
        });
        this.rst.startSet();
        this.rst.initiate();

        const db = this.rst.getPrimary().getDB(jsTestName());
        this.baseColl = db[jsTestName()];
        this.foreignColl = db[jsTestName() + "_foreign"];

        this.settings = makeSettings(this.baseColl);

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
            ]),
        );
        assert.commandWorked(this.baseColl.createIndex({a: 1, b: 1}));
        assert.commandWorked(this.foreignColl.createIndex({a: 1, b: 1}));

        this.pipeline = [
            {$match: {a: {$gt: 0}}},
            {
                $lookup: {
                    from: this.foreignColl.getName(),
                    localField: "a",
                    foreignField: "a",
                    as: "foreignColl",
                },
            },
            {$unwind: "$foreignColl"},
        ];

        // Returns the join plan cache key hash from the queryPlanner section of the explain for
        // 'pipeline', asserting join optimization actually ran.
        this.getKey = () => {
            const explain = this.baseColl.explain().aggregate(this.pipeline);
            assert(joinOptUsed(explain), "expected join optimization to be used", {explain});
            return getQueryPlanner(explain).joinPlanCacheKey;
        };

        const qsutils = new QuerySettingsUtils(db, this.baseColl.getName());
        const query = qsutils.makeAggregateQueryInstance({pipeline: this.pipeline});
        this.getKeyWithSettings = (settings) =>
            qsutils.withQuerySettings(query, settings, this.getKey);
    });

    after(function () {
        this.rst.stopSet();
    });

    it("produces a different hash when index hints are set", function () {
        assert.neq(this.getKey(), this.getKeyWithSettings(this.settings));
    });

    it("produces a different hash when queryFramework is set", function () {
        assert.neq(
            this.getKeyWithSettings({...this.settings, queryFramework: "sbe"}),
            this.getKeyWithSettings(this.settings),
        );
    });

    it("ignores comment when computing the hash", function () {
        assert.eq(
            this.getKeyWithSettings(this.settings),
            this.getKeyWithSettings({...this.settings, comment: "a comment"}),
        );
    });
});
