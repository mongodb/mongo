/*
 * Tests hybrid search with both $scoreFusion and $rankFusion get rejected when inside of $unionWith
 * or $lookup subpipelines on timeseries collections, when the main namespace is not timeseries and
 * the queries are routed through mongos. The main purpose of this test is to make sure different
 * sharding topologies and therefore routing logic correctly validate the subpipeline cannot run on
 * timeseries. General sharded collection coverage is in passthroughs in
 * hybrid_search_in_union_with_lookup_on_ts_rejected.js.
 *
 * @tags: [
 *   requires_timeseries,
 *   requires_sharding,
 * ]
 */

import {getShardNames} from "jstests/libs/cluster_helpers/sharded_cluster_fixture_helpers.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";

const testDb = db.getSiblingDB(jsTestName());
const configDb = db.getSiblingDB("config");
const shardNames = getShardNames(testDb.getMongo());
assert.gte(shardNames.length, 2, "Test requires at least 2 shards");
const primaryShard = shardNames[0];
const otherShard = shardNames[1];

const timeseriesCollName = jsTestName() + "_timeseries";
const nonTimeseriesCollName = jsTestName() + "_nontimeseries";

const rankFusionPipeline = [{$rankFusion: {input: {pipelines: {sortPipeline: [{$sort: {a: 1}}]}}}}];
const scoreFusionPipeline = [
    {
        $scoreFusion: {
            input: {pipelines: {scorePipeline: [{$score: {score: "$a"}}]}, normalization: "none"},
        },
    },
];

function runPipeline(pipeline, collName) {
    // Run the aggregation and fully drain the resulting cursor. The firstBatch in $unionWith might
    // not include timeseries documents, and therefore might succeed, since the subpipeline hasn't
    // been validated yet.
    const initialResponse = testDb.runCommand({aggregate: collName, pipeline, cursor: {}});
    if (!initialResponse.ok) {
        return initialResponse;
    }
    let cursor = initialResponse.cursor;
    while (cursor.id != 0) {
        const getMoreResponse = testDb.runCommand({getMore: cursor.id, collection: collName});
        if (!getMoreResponse.ok) {
            return getMoreResponse;
        }
        cursor = getMoreResponse.cursor;
    }
    return initialResponse;
}

function catalogEntry(fullName) {
    return configDb.collections.findOne({_id: fullName});
}

function shardsForUuid(uuid) {
    return [
        ...new Set(
            configDb.chunks
                .find({uuid})
                .toArray()
                .map((chunk) => chunk.shard),
        ),
    ];
}

function createTimeseries() {
    assert.commandWorked(
        testDb.createCollection(timeseriesCollName, {timeseries: {timeField: "t", metaField: "m"}}),
    );
    assert.commandWorked(
        testDb[timeseriesCollName].insertMany([
            {t: new Date(), m: 1, a: 42, b: 17},
            {t: new Date(), m: 1, a: 44, b: 15},
            {t: new Date(), m: 2, a: 50, b: 1},
            {t: new Date(), m: 2, a: 55, b: 0},
        ]),
    );
}

function createNonTimeseries() {
    assert.commandWorked(testDb.createCollection(nonTimeseriesCollName));
    assert.commandWorked(
        testDb[nonTimeseriesCollName].insertMany([
            {_id: 0, a: 50, b: 20},
            {_id: 2, a: 60, b: 30},
        ]),
    );
}

function moveWholeCollectionTo(collName, shard) {
    assert.commandWorked(
        testDb.adminCommand({moveCollection: testDb[collName].getFullName(), toShard: shard}),
    );
}

function shardCollection({collName, key, split, moveChunk}) {
    const fullName = testDb[collName].getFullName();
    assert.commandWorked(testDb.adminCommand({shardCollection: fullName, key: key}));
    assert.commandWorked(testDb.adminCommand({split: fullName, middle: split}));
    assert.commandWorked(
        testDb.adminCommand({
            moveChunk: fullName,
            find: moveChunk,
            to: otherShard,
            _waitForDelete: true,
        }),
    );
}

function assertTimeseriesUntracked() {
    const fullName = testDb[timeseriesCollName].getFullName();
    assert.commandWorked(db.adminCommand({untrackUnshardedCollection: fullName}));
    assert.eq(catalogEntry(fullName), null, "Timeseries collection should be untracked");
}

function assertCollectionTrackedUnshardedOn(collName, shard) {
    const entry = catalogEntry(testDb[collName].getFullName());
    assert.neq(entry, null, "Collection should be tracked");
    assert.eq(entry.unsplittable, true, "Collection should remain unsharded");
    assert.eq(shardsForUuid(entry.uuid), [shard], "Data should live entirely on " + shard);
}

function assertCollectionShardedAcrossShards(collName) {
    const entry = catalogEntry(testDb[collName].getFullName());
    assert.neq(entry, null, "Collection should be tracked");
    assert.neq(entry.unsplittable, true, "Collection should be sharded");
    const shards = shardsForUuid(entry.uuid);
    assert.gte(shards.length, 2, "Collection should span multiple shards", {shards});
}

const scenarios = [
    // shard1 (non-primary) must send the subpipeline to shard0 (primary).
    // Mongos only knows about the non-TS collection.
    {
        name: "untracked TS; tracked-unsharded non-TS on non-primary shard",
        setup: () => {
            createTimeseries();
            createNonTimeseries();
            moveWholeCollectionTo(nonTimeseriesCollName, otherShard);
            assert.commandWorked(testDb.adminCommand({flushRouterConfig: 1}));
            assertTimeseriesUntracked();
            assertCollectionTrackedUnshardedOn(nonTimeseriesCollName, otherShard);
        },
    },
    // Multiple shards must send subpipeline to shard0 (primary).
    // Mongos only knows about the non-TS collection.
    {
        name: "untracked timeseries; non-timeseries sharded across shards",
        setup: () => {
            createTimeseries();
            createNonTimeseries();
            shardCollection({
                collName: nonTimeseriesCollName,
                key: {_id: 1},
                split: {_id: 1},
                moveChunk: {_id: 2},
            });
            assert.commandWorked(testDb.adminCommand({flushRouterConfig: 1}));
            assertTimeseriesUntracked();
            assertCollectionShardedAcrossShards(nonTimeseriesCollName);
        },
    },

    // Testing local read. shard1 (non-primary) executes both the outer and inner query.
    // Mongos does know about both collections.
    {
        name: "tracked-unsharded timeseries and non-timeseries both on non-primary shard",
        setup: () => {
            createTimeseries();
            createNonTimeseries();
            moveWholeCollectionTo(timeseriesCollName, otherShard);
            moveWholeCollectionTo(nonTimeseriesCollName, otherShard);
            assert.commandWorked(testDb.adminCommand({flushRouterConfig: 1}));
            assertCollectionTrackedUnshardedOn(timeseriesCollName, otherShard);
            assertCollectionTrackedUnshardedOn(nonTimeseriesCollName, otherShard);
        },
    },

    // 2 sharded collections. The pipelines are run on any shard and all shards should use their
    // own catalog to reject the query.
    {
        name: "sharded timeseries; sharded non-timeseries",
        setup: () => {
            testDb[timeseriesCollName].drop();
            testDb[nonTimeseriesCollName].drop();
            createTimeseries();
            createNonTimeseries();
            shardCollection({
                collName: nonTimeseriesCollName,
                key: {_id: 1},
                split: {_id: 1},
                moveChunk: {_id: 2},
            });
            shardCollection({
                collName: timeseriesCollName,
                key: {m: 1},
                split: {meta: 1},
                moveChunk: {meta: 2},
            });
            assert.commandWorked(testDb.adminCommand({flushRouterConfig: 1}));
            assertCollectionShardedAcrossShards(nonTimeseriesCollName);
            assertCollectionShardedAcrossShards(timeseriesCollName);
        },
    },
];

describe("hybrid search rejected in subpipelines on timeseries collections across sharding topologies", function () {
    before(function () {
        assert.commandWorked(
            testDb.adminCommand({enableSharding: testDb.getName(), primaryShard: primaryShard}),
        );
    });

    for (const scenario of scenarios) {
        describe(scenario.name, function () {
            before(function () {
                testDb[timeseriesCollName].drop();
                testDb[nonTimeseriesCollName].drop();
                scenario.setup();
            });

            after(function () {
                testDb[timeseriesCollName].drop();
                testDb[nonTimeseriesCollName].drop();
            });

            // TODO SERVER-121094 Remove error codes '10787900', '10787901' once all validation is
            // done in the lite parsed pipeline.
            it("$unionWith subpipeline on timeseries", function () {
                // The outer query runs on the shard(s) that own the non-timeseries collection, which must
                // dispatch the subpipeline to the shard that owns the timeseries collection.
                let rankFusionUnionWithStage = {
                    $unionWith: {coll: timeseriesCollName, pipeline: rankFusionPipeline},
                };
                assert.commandFailedWithCode(
                    runPipeline([rankFusionUnionWithStage], nonTimeseriesCollName),
                    [10787900, 10787901, 12093200],
                );

                let scoreFusionUnionWithStage = {
                    $unionWith: {coll: timeseriesCollName, pipeline: scoreFusionPipeline},
                };
                assert.commandFailedWithCode(
                    runPipeline([scoreFusionUnionWithStage], nonTimeseriesCollName),
                    [10787900, 10787901, 12093200],
                );
            });

            it("$unionWith nested subpipeline on timeseries", function () {
                let rankFusionUnionWithStage = {
                    $unionWith: {coll: timeseriesCollName, pipeline: rankFusionPipeline},
                };
                let nestedRankFusionUnionWithStage = {
                    $unionWith: {coll: nonTimeseriesCollName, pipeline: [rankFusionUnionWithStage]},
                };
                assert.commandFailedWithCode(
                    runPipeline([nestedRankFusionUnionWithStage], nonTimeseriesCollName),
                    [10787900, 10787901, 12093200],
                );

                let scoreFusionUnionWithStage = {
                    $unionWith: {coll: timeseriesCollName, pipeline: scoreFusionPipeline},
                };
                let nestedScoreFusionUnionWithStage = {
                    $unionWith: {
                        coll: nonTimeseriesCollName,
                        pipeline: [scoreFusionUnionWithStage],
                    },
                };
                assert.commandFailedWithCode(
                    runPipeline([nestedScoreFusionUnionWithStage], nonTimeseriesCollName),
                    [10787900, 10787901, 12093200],
                );
            });

            it("$lookup subpipeline on timeseries", function () {
                // The outer query runs on the shard(s) that own the non-timeseries collection,
                // which must dispatch the subpipeline to the shard that owns the timeseries collection.
                let rankFusionLookupStage = {
                    $lookup: {from: timeseriesCollName, pipeline: rankFusionPipeline, as: "out"},
                };
                assert.commandFailedWithCode(
                    runPipeline([rankFusionLookupStage], nonTimeseriesCollName),
                    [10787900, 10787901, 12093200],
                );

                let scoreFusionLookupStage = {
                    $lookup: {from: timeseriesCollName, pipeline: scoreFusionPipeline, as: "out"},
                };
                assert.commandFailedWithCode(
                    runPipeline([scoreFusionLookupStage], nonTimeseriesCollName),
                    [10787900, 10787901, 12093200],
                );
            });

            it("nested $lookup subpipeline on timeseries", function () {
                let rankFusionLookupStage = {
                    $lookup: {from: timeseriesCollName, pipeline: rankFusionPipeline, as: "out"},
                };
                let nestedLookupRankFusionStage = {
                    $lookup: {
                        from: nonTimeseriesCollName,
                        pipeline: [rankFusionLookupStage],
                        as: "out",
                    },
                };
                assert.commandFailedWithCode(
                    runPipeline([nestedLookupRankFusionStage], nonTimeseriesCollName),
                    [10787900, 10787901, 12093200],
                );

                let scoreFusionLookupStage = {
                    $lookup: {from: timeseriesCollName, pipeline: scoreFusionPipeline, as: "out"},
                };
                let nestedLookupScoreFusionStage = {
                    $lookup: {
                        from: nonTimeseriesCollName,
                        pipeline: [scoreFusionLookupStage],
                        as: "out",
                    },
                };
                assert.commandFailedWithCode(
                    runPipeline([nestedLookupScoreFusionStage], nonTimeseriesCollName),
                    [10787900, 10787901, 12093200],
                );
            });
        });
    }
});
