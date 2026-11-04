/**
 * Asserts that the 'eligibleForStreamingGroup' plan shape counter in query stats counts exactly
 * the plans whose pushed-down $group could have run as a streaming $group, on a standalone and on
 * a sharded cluster.
 *
 * @tags: [
 *   requires_fcv_91,
 *   requires_sharding,
 * ]
 */
import {
    getQueryStats,
    getQueryStatsServerParameters,
    resetQueryStatsStore,
} from "jstests/libs/query/query_stats_utils.js";
import {checkSbeStatus, kSbeRestricted} from "jstests/libs/query/sbe_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const docs = Array.from({length: 20}, (_, i) => ({
    _id: i,
    a: i % 5,
    b: i % 3,
    c: i,
    s: "str" + (i % 4),
    x: {y: i % 2},
}));

function makeHelpers(db, collName) {
    const conn = db.getMongo();
    const coll = db[collName];

    function nodeCounters(pipeline, options = {}, runs = 1) {
        resetQueryStatsStore(conn, "1MB");
        for (let i = 0; i < runs; i++) {
            coll.aggregate(pipeline, options).itcount();
        }
        const stats = getQueryStats(conn, {collName});
        assert.eq(1, stats.length, "expected exactly one query stats entry", {stats});
        const metrics = stats[0].metrics;
        const queryPlanner = metrics.hasOwnProperty("queryPlanner")
            ? metrics.queryPlanner
            : metrics;
        return queryPlanner.planShapeCounters?.nodes ?? {};
    }

    function assertCounts(nodes, {group, eligible}, context) {
        assert.eq(group, Number(nodes.group ?? 0), {context, nodes});
        assert.eq(eligible, Number(nodes.eligibleForStreamingGroup ?? 0), {context, nodes});
    }

    function setIndexes(indexes) {
        assert.commandWorked(coll.dropIndexes());
        for (const [keyPattern, options] of indexes) {
            assert.commandWorked(coll.createIndex(keyPattern, options ?? {}));
        }
    }

    function runCases(cases) {
        const mismatches = [];
        for (const {name, indexes, pipeline, options, group, eligible} of cases) {
            setIndexes(indexes);
            const nodes = nodeCounters(pipeline, options);
            const actual = {
                group: Number(nodes.group ?? 0),
                eligible: Number(nodes.eligibleForStreamingGroup ?? 0),
            };
            if (actual.group !== group || actual.eligible !== eligible) {
                mismatches.push({name, expected: {group, eligible}, actual, nodes});
            }
        }
        assert.eq([], mismatches);
    }

    return {coll, nodeCounters, assertCounts, setIndexes, runCases};
}

function eligibleCase(name, indexes, pipeline, options) {
    return {name, indexes, pipeline, options, group: 1, eligible: 1};
}

function ineligibleCase(name, indexes, pipeline, options) {
    return {name, indexes, pipeline, options, group: 1, eligible: 0};
}

const kSum = {total: {$sum: "$c"}};

const standaloneCases = [
    eligibleCase(
        "covered index sort",
        [[{a: 1}]],
        [{$match: {a: {$gte: 0}}}, {$group: {_id: "$a", ...kSum}}],
    ),
    eligibleCase(
        "index without a predicate",
        [[{a: 1}]],
        [{$sort: {a: 1}}, {$group: {_id: "$a", ...kSum}}],
    ),
    eligibleCase(
        "descending index",
        [[{a: -1}]],
        [{$match: {a: {$gte: 0}}}, {$group: {_id: "$a", ...kSum}}],
    ),
    eligibleCase(
        "leading sort in the opposite direction",
        [[{a: 1}]],
        [{$sort: {a: -1}}, {$group: {_id: "$a", ...kSum}}],
    ),
    eligibleCase(
        "key after an equality prefix",
        [[{a: 1, b: 1}]],
        [{$match: {a: 2}}, {$group: {_id: "$b", ...kSum}}],
    ),
    eligibleCase(
        "key is the leading field of a compound index",
        [[{a: 1, b: 1}]],
        [{$match: {a: {$gte: 0}}}, {$group: {_id: "$a", ...kSum}}],
    ),
    eligibleCase(
        "inclusion projection before the group",
        [[{a: 1}]],
        [
            {$match: {a: {$gte: 0}}},
            {$project: {_id: 0, a: 1, c: 1}},
            {$group: {_id: "$a", ...kSum}},
        ],
    ),
    eligibleCase(
        "dotted key",
        [[{"x.y": 1}]],
        [{$match: {"x.y": {$gte: 0}}}, {$group: {_id: "$x.y", ...kSum}}],
    ),
    eligibleCase(
        "accumulators other than $sum",
        [[{a: 1}]],
        [
            {$match: {a: {$gte: 0}}},
            {
                $group: {
                    _id: "$a",
                    all: {$push: "$c"},
                    distinct: {$addToSet: "$b"},
                    mean: {$avg: "$c"},
                    low: {$min: "$c"},
                    high: {$max: "$c"},
                    head: {$first: "$c"},
                    tail: {$last: "$c"},
                    n: {$count: {}},
                },
            },
        ],
    ),
    eligibleCase(
        "group followed by other stages",
        [[{a: 1}]],
        [
            {$match: {a: {$gte: 0}}},
            {$group: {_id: "$a", ...kSum}},
            {$sort: {total: 1}},
            {$limit: 2},
        ],
    ),

    {
        name: "no accumulators, planned as a DISTINCT_SCAN",
        indexes: [[{a: 1}]],
        pipeline: [{$match: {a: {$gte: 0}}}, {$group: {_id: "$a"}}],
        group: 0,
        eligible: 0,
    },

    ineligibleCase(
        "partial index, which gives no arrayness",
        [[{a: 1}, {partialFilterExpression: {a: {$gte: 0}}}]],
        [{$match: {a: {$gte: 0}}}, {$group: {_id: "$a", ...kSum}}],
    ),
    ineligibleCase(
        "sparse index, which gives no arrayness",
        [[{a: 1}, {sparse: true}]],
        [{$match: {a: {$gte: 0}}}, {$group: {_id: "$a", ...kSum}}],
    ),
    ineligibleCase(
        "wildcard index, which gives no arrayness",
        [[{"$**": 1}]],
        [{$match: {a: {$gte: 0}}}, {$group: {_id: "$a", ...kSum}}],
    ),
    ineligibleCase(
        "collection scan",
        [],
        [{$match: {a: {$gte: 0}}}, {$group: {_id: "$a", ...kSum}}],
    ),
    ineligibleCase(
        "index on a different field",
        [[{b: 1}]],
        [{$match: {b: {$gte: 0}}}, {$group: {_id: "$a", ...kSum}}],
    ),
    ineligibleCase(
        "key is not the leading sort field",
        [[{a: 1, b: 1}]],
        [{$match: {a: {$gte: 0}}}, {$group: {_id: "$b", ...kSum}}],
    ),
    ineligibleCase(
        "equality on the key",
        [[{a: 1}]],
        [{$match: {a: 2}}, {$group: {_id: "$a", ...kSum}}],
    ),
    ineligibleCase(
        "projection overwrites the key",
        [[{a: 1}]],
        [{$match: {a: 2}}, {$project: {_id: 0, a: "$c"}}, {$group: {_id: "$a"}}],
    ),
    ineligibleCase(
        "projection renames a constant into the key",
        [[{a: 1, b: 1}]],
        [{$match: {a: 2}}, {$project: {_id: 0, b: "$a"}}, {$group: {_id: "$b"}}],
    ),
    ineligibleCase(
        "object key",
        [[{a: 1}]],
        [{$match: {a: {$gte: 0}}}, {$group: {_id: {k: "$a"}, ...kSum}}],
    ),
    ineligibleCase(
        "array key",
        [[{a: 1}]],
        [{$match: {a: {$gte: 0}}}, {$group: {_id: ["$a"], ...kSum}}],
    ),
    ineligibleCase(
        "computed key",
        [[{a: 1}]],
        [{$match: {a: {$gte: 0}}}, {$group: {_id: {$add: ["$a", 1]}, ...kSum}}],
    ),
    ineligibleCase(
        "constant key",
        [[{a: 1}]],
        [{$match: {a: {$gte: 0}}}, {$group: {_id: null, ...kSum}}],
    ),
    ineligibleCase(
        "whole document key",
        [[{a: 1}]],
        [{$match: {a: {$gte: 0}}}, {$group: {_id: "$$ROOT"}}],
    ),
    ineligibleCase(
        "variable key",
        [[{a: 1}]],
        [{$match: {a: {$gte: 0}}}, {$group: {_id: "$$v", ...kSum}}],
        {let: {v: 1}},
    ),
    ineligibleCase(
        "non-simple collation",
        [[{s: 1}, {collation: {locale: "fr"}}]],
        [{$match: {s: {$gte: ""}}}, {$group: {_id: "$s", ...kSum}}],
        {collation: {locale: "fr"}},
    ),
    ineligibleCase(
        "hashed index",
        [[{a: "hashed"}]],
        [{$match: {a: 2}}, {$group: {_id: "$a", ...kSum}}],
    ),
    ineligibleCase(
        "group is not the first pushed-down stage",
        [[{a: 1}]],
        [
            {$match: {a: {$gte: 0}}},
            {$lookup: {from: "foreign", localField: "a", foreignField: "a", as: "f"}},
            {$group: {_id: "$a", ...kSum}},
        ],
    ),
];

function runStandaloneCases(db) {
    const collName = jsTestName();
    const {coll, nodeCounters, assertCounts, setIndexes, runCases} = makeHelpers(db, collName);
    coll.drop();
    assert.commandWorked(coll.insert(docs));
    assert.commandWorked(db.foreign.insert([{a: 1}, {a: 2}]));

    runCases(standaloneCases);

    const eligiblePipeline = [{$match: {a: {$gte: 0}}}, {$group: {_id: "$a", ...kSum}}];

    // Repeated runs count once each, including runs whose plan comes from the plan cache. Two
    // indexes on 'a' make the planner multiplan and cache the winner.
    {
        setIndexes([[{a: 1}], [{a: 1, c: 1}]]);
        assertCounts(
            nodeCounters(eligiblePipeline, {}, 4),
            {group: 4, eligible: 4},
            "repeated runs",
        );
        const cacheEntries = coll.aggregate([{$planCacheStats: {}}]).toArray();
        assert(
            cacheEntries.some((entry) => entry.isActive),
            "expected an active plan cache entry",
            {cacheEntries},
        );
        assertCounts(
            nodeCounters(eligiblePipeline),
            {group: 1, eligible: 1},
            "run from the plan cache",
        );
    }

    // Once the key's index becomes multikey, the plan cache is cleared and the next run is
    // planned with the new arrayness, so it is not counted as eligible.
    {
        setIndexes([[{a: 1}], [{a: 1, c: 1}]]);
        assertCounts(
            nodeCounters(eligiblePipeline, {}, 3),
            {group: 3, eligible: 3},
            "before multikey",
        );
        assert.commandWorked(
            coll.insert({_id: "multikey", a: [1, 2], b: 0, c: 0, s: "", x: {y: 0}}),
        );
        assertCounts(nodeCounters(eligiblePipeline), {group: 1, eligible: 0}, "after multikey");
        assert.commandWorked(coll.deleteOne({_id: "multikey"}));
    }

    // A multikey index on a different field leaves the key eligible.
    {
        setIndexes([[{a: 1, b: 1}]]);
        assert.commandWorked(
            coll.insert({_id: "multikeyB", a: 1, b: [1, 2], c: 0, s: "", x: {y: 0}}),
        );
        assertCounts(
            nodeCounters(eligiblePipeline),
            {group: 1, eligible: 1},
            "other field multikey",
        );
        assertCounts(
            nodeCounters([{$match: {a: 1}}, {$group: {_id: "$b"}}]),
            {group: 1, eligible: 0},
            "key is multikey",
        );
        assert.commandWorked(coll.deleteOne({_id: "multikeyB"}));
    }

    // The flag is a kill switch: with it off the $group is still counted, but never as eligible.
    {
        setIndexes([[{a: 1}]]);
        assert.commandWorked(
            db.adminCommand({setParameter: 1, featureFlagStreamingGroupEligibilityCounter: false}),
        );
        assertCounts(nodeCounters(eligiblePipeline), {group: 1, eligible: 0}, "flag off");
        assert.commandWorked(
            db.adminCommand({setParameter: 1, featureFlagStreamingGroupEligibilityCounter: true}),
        );
        assertCounts(nodeCounters(eligiblePipeline), {group: 1, eligible: 1}, "flag back on");
    }

    // A $group that runs in the classic engine has no GROUP node, so neither counter is set.
    {
        setIndexes([[{a: 1}]]);
        assert.commandWorked(
            db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "forceClassicEngine"}),
        );
        assertCounts(nodeCounters(eligiblePipeline), {group: 0, eligible: 0}, "classic engine");
        assert.commandWorked(
            db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "trySbeRestricted"}),
        );
    }
}

function runShardedCases(st) {
    const db = st.s.getDB("test");
    assert.commandWorked(db.foreign.insert([{a: 1}, {a: 2}]));

    // An unsharded collection runs the whole pipeline on its primary shard.
    {
        const {coll, nodeCounters, assertCounts, setIndexes} = makeHelpers(db, "unsharded");
        assert.commandWorked(coll.insert(docs));
        setIndexes([[{a: 1}]]);
        assertCounts(
            nodeCounters([{$match: {a: {$gte: 0}}}, {$group: {_id: "$a", ...kSum}}]),
            {group: 1, eligible: 1},
            "unsharded collection",
        );
    }

    // On a sharded collection, a $group that is split between the shards and the router is
    // marked willBeMerged on the shards, so it is not eligible.
    {
        const {coll, nodeCounters, assertCounts, setIndexes} = makeHelpers(db, "sharded");
        st.shardColl(coll, {c: 1}, {c: 10});
        assert.commandWorked(coll.insert(docs));
        setIndexes([[{a: 1}]]);
        assertCounts(
            nodeCounters([{$match: {a: {$gte: 0}}}, {$group: {_id: "$a", ...kSum}}]),
            {group: 2, eligible: 0},
            "split group",
        );
    }

    // A $group on the shard key runs entirely on the shards, with no merge on the router.
    {
        const {coll, nodeCounters, assertCounts} = makeHelpers(db, "sharded");
        assertCounts(
            nodeCounters([{$match: {c: {$gte: 0}}}, {$group: {_id: "$c", ...kSum}}]),
            {group: 2, eligible: 2},
            "group on the shard key",
        );
    }
}

{
    const conn = MongoRunner.runMongod(getQueryStatsServerParameters());
    const db = conn.getDB("test");
    if (checkSbeStatus(db) === kSbeRestricted) {
        runStandaloneCases(db);
    } else {
        jsTest.log.info("Skipping the standalone cases: they need $group pushed down to SBE");
    }
    MongoRunner.stopMongod(conn);
}

{
    const st = new ShardingTest({
        shards: 2,
        mongosOptions: getQueryStatsServerParameters(),
        rsOptions: getQueryStatsServerParameters(),
    });
    if (checkSbeStatus(st.s.getDB("test")) === kSbeRestricted) {
        runShardedCases(st);
    } else {
        jsTest.log.info("Skipping the sharded cases: they need $group pushed down to SBE");
    }
    st.stop();
}
