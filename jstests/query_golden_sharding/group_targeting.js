/**
 * Tests the results and explain for grouping on a potentially sharded collection. It is especially
 * useful to see in which cases a $group can be pushed down to the shards.
 *
 * @tags: [
 *   featureFlagShardFilteringDistinctScan,
 * ]
 */

import {
    line,
    outputAggregationPlanAndResults,
    section,
    subSection
} from "jstests/libs/pretty_md.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const shardingTest = new ShardingTest({shards: 2});

const db = shardingTest.getDB("test");

// Enable sharding.
const primaryShard = shardingTest.shard0.shardName;
const otherShard = shardingTest.shard1.shardName;
assert.commandWorked(shardingTest.s0.adminCommand({enableSharding: db.getName(), primaryShard}));

{
    section("Pushdown $group on single shard-key");
    const coll = db[jsTestName()];
    coll.drop();
    coll.createIndex({shardKey: 1});
    coll.insertMany([
        {_id: 1, shardKey: "shard0_1", otherField: "a"},
        {_id: 1.5, shardKey: "shard0_1", otherField: "A"},
        {_id: 2, shardKey: "shard0_2", otherField: "b"},
        {_id: 2.5, shardKey: "sHaRd0_2", otherField: "b"},
        {_id: 3, shardKey: "shard0_3", otherField: "c"},
        {_id: 3.5, shardKey: "shard0_3", otherField: "C"}
    ]);

    // Move "shard_1*" chunk to otherShard.
    assert.commandWorked(
        shardingTest.s0.adminCommand({shardCollection: coll.getFullName(), key: {shardKey: 1}}));
    assert.commandWorked(
        shardingTest.s.adminCommand({split: coll.getFullName(), middle: {shardKey: "shard1"}}));
    assert.commandWorked(shardingTest.s.adminCommand(
        {moveChunk: coll.getFullName(), find: {shardKey: "shard1_1"}, to: otherShard}));

    // Insert these docs after moving the chunk to avoid orphans.
    coll.insertMany([
        {_id: 4, shardKey: "shard1_1", otherField: "a"},
        {_id: 4.5, shardKey: "shard1_1", otherField: "A"},
        {_id: 5, shardKey: "shard1_2", otherField: "b"},
        {_id: 6, shardKey: "shard1_3", otherField: "c"},
        // Note: this is actually on shard 0 because "shARD1_3" < "shard1_1"!
        {_id: 6.5, shardKey: "shARD1_3", otherField: "c"}
    ]);

    subSection("Pushdown works for simple $group where _id == shard key");
    outputAggregationPlanAndResults(coll, [{$group: {_id: "$shardKey"}}]);

    subSection("Pushdown works for pipeline prefix $group that is eligible for distinct scan");
    outputAggregationPlanAndResults(coll, [
        {
            $group: {
                _id: "$shardKey",
                otherField: {$top: {output: "$otherField", sortBy: {shardKey: 1}}}
            }
        },
        {$match: {_id: {$lte: "shard1_1"}}},
        {$sort: {_id: 1}}
    ]);

    section("Pushdown $group executes $avg correctly");
    outputAggregationPlanAndResults(coll, [{$group: {_id: "$shardKey", avg: {$avg: "$_id"}}}]);

    subSection("Pushdown works for simple $group where _id == shard key with a simple rename");
    outputAggregationPlanAndResults(
        coll, [{$project: {renamedShardKey: "$shardKey"}}, {$group: {_id: "$renamedShardKey"}}]);

    section("Pushdown works for simple $group on superset of shard-key");
    outputAggregationPlanAndResults(coll, [{$group: {_id: ["$shardKey", "$_id"]}}]);

    subSection("Pushdown works for pipeline prefix $group on superset of shard key");
    outputAggregationPlanAndResults(coll, [
        {
            $group: {
                _id: ["$shardKey", "$_id"],
                otherField: {$top: {output: "$otherField", sortBy: {shardKey: 1}}}
            }
        },
        {$match: {_id: {$lte: "shard1_1"}}},
        {$sort: {_id: 1}}
    ]);

    subSection("Pushdown works for simple $group on superset of shard key + rename");
    outputAggregationPlanAndResults(coll, [
        {$project: {renamedShardKey: "$shardKey"}},
        {$group: {_id: {secretlyShardKey: "$renamedShardKey", actualId: "$_id"}}}
    ]);

    subSection("Only partial pushdown of $group on key derived from shard-key");
    outputAggregationPlanAndResults(coll, [{$group: {_id: {"$min": ["$shardKey", 1]}}}]);

    subSection("Only partial pushdown of $group on key derived from shard-key, more complex case");
    outputAggregationPlanAndResults(coll, [
        {
            $group: {
                _id: {"$min": ["$shardKey", 1]},
                otherField: {$top: {output: "$otherField", sortBy: {shardKey: 1}}}
            }
        },
        {$match: {_id: {$lte: "shard1_1"}}},
        {$sort: {_id: 1}}
    ]);

    subSection(
        "Only partial pushdown of $group on key derived from shard-key, dependency on other field");
    outputAggregationPlanAndResults(coll, [{$group: {_id: {"$min": ["$shardKey", "$_id"]}}}]);

    subSection("With multiple $groups, pushdown first group when on shard key");
    outputAggregationPlanAndResults(
        coll,
        [{$group: {_id: {key: "$shardKey", other: "$otherField"}}}, {$group: {_id: "$_id.other"}}]);

    subSection("Multiple groups on shard key- could actually push both down");
    outputAggregationPlanAndResults(
        coll, [{$group: {_id: {key: "$shardKey", other: "$otherField"}}}, {$group: {_id: "$_id"}}]);

    subSection("Fully pushed down $group, followed by partially pushed down group");
    outputAggregationPlanAndResults(coll, [
        {$group: {_id: "$shardKey", avg: {$avg: "$_id"}}},
        {$group: {_id: "$avg", num: {$count: {}}}}
    ]);

    subSection("Don't fully pushdown $group on non-shard-key");
    outputAggregationPlanAndResults(coll, [{$group: {_id: "$otherField"}}]);
    outputAggregationPlanAndResults(
        coll, [{$project: {shardKey: "$otherField"}}, {$group: {_id: "$shardKey"}}]);

    subSection("Don't fully pushdown $group when the shard key is not preserved");
    outputAggregationPlanAndResults(coll,
                                    [{$project: {otherField: 1}}, {$group: {_id: "$shardKey"}}]);

    subSection("Don't fully pushdown $group when the shard key is overwritten by $addFields");
    outputAggregationPlanAndResults(
        coll, [{$addFields: {shardKey: "$otherField"}}, {$group: {_id: "$shardKey"}}]);

    subSection("Don't fully pushdown $group for _id on $$ROOT");
    outputAggregationPlanAndResults(coll, [{$group: {_id: "$$ROOT"}}]);

    subSection("Could pushdown $group for _id on $$ROOT.shardKey");
    outputAggregationPlanAndResults(coll, [{$group: {_id: "$$ROOT.shardKey"}}]);

    // We shouldn't push down in this case since the aggregation collaton is coarser than the shard
    // key (i.e. we expect fewer groups than we would get if we were to pushdown).
    subSection("Don't push down $group on shard key if collation of aggregation is non-simple");
    line("Note: If we have duplicate _ids in the output, that signals a bug here.");
    outputAggregationPlanAndResults(coll,
                                    [
                                        {$group: {_id: "$shardKey"}},
                                        // Necessary, since otherwise one of our group keys could be
                                        // either "shARD1_3" or "shard1_3"!
                                        {$addFields: {_id: {$toLower: "$_id"}}}
                                    ],
                                    {collation: {locale: "en_US", strength: 2}});

    subSection("Don't push $group fully down if shard-key field was added later.");
    outputAggregationPlanAndResults(coll, [
        {$project: {shardKey: 0}},
        {$addFields: {shardKey: "$otherField"}},
        {$group: {_id: "$shardKey"}}
    ]);
}

{
    section("Sharded collection with compound key");
    // Repeat tests above for compound shard key case.
    const coll = db[`${jsTestName()}_compound`];
    coll.drop();
    coll.createIndex({sk0: 1, sk1: 1, sk2: 1});
    // Move a chunk to otherShard.
    assert.commandWorked(shardingTest.s0.adminCommand(
        {shardCollection: coll.getFullName(), key: {sk0: 1, sk1: 1, sk2: 1}}));
    assert.commandWorked(shardingTest.s.adminCommand(
        {split: coll.getFullName(), middle: {sk0: "s0/1", sk1: 1, sk2: "h"}}));
    assert.commandWorked(shardingTest.s.adminCommand(
        {moveChunk: coll.getFullName(), find: {sk0: "s0/1", sk1: 1, sk2: "h"}, to: otherShard}));

    coll.insertMany([
        {_id: 1, sk0: "s0", sk1: 1, sk2: "a", otherField: "abc"},
        {_id: 1.5, sk0: "s0", sk1: 1, sk2: "a", otherField: "GEH"},
        {_id: 2, sk0: "s0", sk1: 2, sk2: "a", otherField: "def"},
        {_id: 3, sk0: "s0/1", sk1: 2, sk2: "b", otherField: "abc"},
        {_id: 3.5, sk0: "s0/1", sk1: 2, sk2: "b", otherField: "ABC"},
        {_id: 4, sk0: "s0/1", sk1: 2, sk2: "z", otherField: "abc"},
        {_id: 5, sk0: "s0/1", sk1: 1, sk2: "b", otherField: "def"},
        {_id: 6, sk0: "s0/1", sk1: 1, sk2: "z", otherField: "abc"},
        {_id: 7, sk0: "s1", sk1: 3, sk2: "b", otherField: "def"},
        {_id: 7.5, sk0: "s1", sk1: 3, sk2: "b", otherField: "DEF"},
        {_id: 8, sk0: "s1", sk1: 5, sk2: "c", otherField: "ghi"},
    ]);

    subSection("Pushdown works for simple $group where _id == shard key");
    outputAggregationPlanAndResults(coll,
                                    [{$group: {_id: {sk0: "$sk0", sk1: "$sk1", sk2: "$sk2"}}}]);

    subSection("Pushdown works for simple $group where _id == shard key + accumulators");
    outputAggregationPlanAndResults(coll, [{
                                        $group: {
                                            _id: {sk0: "$sk0", sk1: "$sk1", sk2: "$sk2"},
                                            sumSk1: {$sum: "$sk1"},
                                            setSk2: {$addToSet: "$sk2"}
                                        }
                                    }]);

    subSection("Pushdown works for simple $group where _id == shard key + can use distinct scan");
    outputAggregationPlanAndResults(
        coll, [{
            $group: {
                _id: {sk0: "$sk0", sk1: "$sk1", sk2: "$sk2"},
                root: {$bottom: {sortBy: {sk0: 1, sk1: 1, sk2: 1}, output: "$$ROOT"}}
            }
        }]);

    subSection("Pushdown works for simple $group where _id == shard key with a simple rename");
    outputAggregationPlanAndResults(coll, [
        {$project: {sk0Renamed: "$sk0", sk1Renamed: "$sk1", sk2Renamed: "$sk2", _id: 0}},
        {$group: {_id: {sk0: "$sk0Renamed", sk1: "$sk1Renamed", sk2: "$sk2Renamed"}}}
    ]);

    subSection(
        "Only partial pushdown for simple $group where _id == shard key with a non-simple rename");
    outputAggregationPlanAndResults(coll, [
        {
            $project:
                {sk0Renamed: "$sk0", sk1Renamed: {$add: [1, "$sk1"]}, sk2Renamed: "$sk2", _id: 0}
        },
        {$group: {_id: {sk0: "$sk0Renamed", sk1: "$sk1Renamed", sk2: "$sk2Renamed"}}}
    ]);

    subSection("Only partial pushdown for simple $group where _id is a subset of the shard key");
    outputAggregationPlanAndResults(coll, [{$group: {_id: {sk0: "$sk0", sk2: "$sk2"}}}]);

    subSection(
        "Pushdown works for simple $group where _id is a superset of the shard key with a simple rename");
    outputAggregationPlanAndResults(coll, [
        {
            $project:
                {sk0Renamed: "$sk0", sk1Renamed: "$sk1", sk2Renamed: "$sk2", _id: 0, otherField: 1}
        },
        {
            $group: {
                _id: {
                    sk0: "$sk0Renamed",
                    sk1: "$sk1Renamed",
                    sk2: "$sk2Renamed",
                    otherField: "$otherField"
                }
            }
        }
    ]);

    subSection("Multiple groups on shard key- could actually push both down");
    outputAggregationPlanAndResults(
        coll, [{$group: {_id: {sk0: "$sk0", sk1: "$sk1", sk2: "$sk2"}}}, {$group: {_id: "$_id"}}]);

    subSection(
        "Push down $group which includes a field that is not the shardKey and is not a simple rename of some other field");
    outputAggregationPlanAndResults(coll, [
        {$addFields: {complex: {$rand: {}}}},
        {$group: {_id: {sk0: "$sk0", complex: "$complex", sk1: "$sk1", sk2: "$sk2"}}},
        {$group: {_id: {sk0: "$_id.sk0", sk1: "$_id.sk1", sk2: "$_id.sk2"}}},
    ]);
}

shardingTest.stop();
