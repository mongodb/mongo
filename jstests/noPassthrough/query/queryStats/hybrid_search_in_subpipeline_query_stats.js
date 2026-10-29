/**
 * Tests that query shapes containing hybrid search in a $lookup or $unionWith subpipeline can be
 * read back from $queryStats, on both a standalone and a sharded cluster.
 *
 * Hybrid search desugaring marks the enclosing stage with the internal-only
 * $_internalIsHybridSearch field. If that marker is serialized into the query shape, reading it
 * back re-parses the shape under an external client, which rejects the internal field.
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const queryStatsParams = {
    internalQueryStatsSampleRate: 1,
    internalQueryStatsCacheSize: "10MB",
};

const scoreFusion = {
    $scoreFusion: {
        input: {
            pipelines: {
                a: [{$score: {score: "$x", normalization: "minMaxScaler"}}, {$sort: {x: -1}}],
                b: [{$score: {score: "$y", normalization: "minMaxScaler"}}, {$sort: {y: -1}}],
            },
            normalization: "none",
        },
        combination: {method: "avg"},
    },
};

const rankFusion = {
    $rankFusion: {input: {pipelines: {a: [{$sort: {x: -1}}], b: [{$sort: {y: -1}}]}}},
};

// Each case gets its own outer collection so the $queryStats read can be filtered down to just the
// entry that case produced.
const cases = [
    {
        name: "$scoreFusion in $lookup",
        outer: "lookupScoreFusion",
        pipeline: [{$lookup: {from: "target", pipeline: [scoreFusion], as: "out"}}],
    },
    {
        name: "$rankFusion in $lookup",
        outer: "lookupRankFusion",
        pipeline: [{$lookup: {from: "target", pipeline: [rankFusion], as: "out"}}],
    },
    {
        name: "$scoreFusion in $unionWith",
        outer: "unionScoreFusion",
        pipeline: [{$unionWith: {coll: "target", pipeline: [scoreFusion]}}],
    },
    {
        name: "$rankFusion in $unionWith",
        outer: "unionRankFusion",
        pipeline: [{$unionWith: {coll: "target", pipeline: [rankFusion]}}],
    },
];

function populate(conn) {
    const testDb = conn.getDB(jsTestName());
    assert.commandWorked(
        testDb.target.insertMany([
            {_id: 1, x: 1, y: 50},
            {_id: 2, x: 5, y: 40},
            {_id: 3, x: 3, y: 30},
            {_id: 4, x: 8, y: 20},
            {_id: 5, x: 2, y: 10},
            {_id: 6, x: 6, y: 60},
        ]),
    );
    for (const testCase of cases) {
        assert.commandWorked(testDb[testCase.outer].insertOne({_id: 0, isOuter: true}));
    }
    return testDb;
}

function assertShapeIsReadable(testDb, testCase) {
    assert.commandWorked(
        testDb.runCommand({aggregate: testCase.outer, pipeline: testCase.pipeline, cursor: {}}),
    );

    const res = testDb.getSiblingDB("admin").runCommand({
        aggregate: 1,
        pipeline: [{$queryStats: {}}, {$match: {"key.queryShape.cmdNs.coll": testCase.outer}}],
        cursor: {},
    });
    assert.commandWorked(res);

    const entries = res.cursor.firstBatch;
    assert.gt(entries.length, 0, "no queryStats entry found", {res});
    assert(
        !tojson(entries).includes("$_internalIsHybridSearch"),
        "internal hybrid search marker leaked into the query shape",
        {entries},
    );
}

describe("$queryStats for hybrid search in subpipelines", function () {
    describe("on a standalone", function () {
        let conn;
        let testDb;

        before(function () {
            conn = MongoRunner.runMongod({setParameter: queryStatsParams});
            testDb = populate(conn);
        });

        after(function () {
            MongoRunner.stopMongod(conn);
        });

        for (const testCase of cases) {
            it(`${testCase.name} re-parses its stored query shape`, function () {
                assertShapeIsReadable(testDb, testCase);
            });
        }
    });

    describe("on a sharded cluster", function () {
        let st;
        let testDb;

        before(function () {
            // $queryStats is node-local, so a single mongos guarantees the aggregation and the
            // subsequent $queryStats read land on the same node.
            st = new ShardingTest({
                mongos: 1,
                shards: 2,
                rs: {nodes: 1},
                mongosOptions: {setParameter: queryStatsParams},
                rsOptions: {setParameter: queryStatsParams},
            });
            assert.commandWorked(
                st.s.adminCommand({enableSharding: jsTestName(), primaryShard: st.shard0.name}),
            );
            assert.commandWorked(
                st.s.adminCommand({
                    shardCollection: `${jsTestName()}.target`,
                    key: {_id: "hashed"},
                }),
            );
            testDb = populate(st.s);
        });

        after(function () {
            st.stop();
        });

        for (const testCase of cases) {
            it(`${testCase.name} re-parses its stored query shape`, function () {
                assertShapeIsReadable(testDb, testCase);
            });
        }
    });
});
