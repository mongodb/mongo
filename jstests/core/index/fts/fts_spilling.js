// Test that $text query can spill when textScore is needed.
// @tags: [
//   not_allowed_with_signed_security_token,
//   requires_fcv_81,
//   requires_persistence,
//   requires_non_retryable_commands,
// ]

import {arrayDiff} from "jstests/aggregation/extras/utils.js";
import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {getExecutionStages, getPlanStage} from "jstests/libs/query/analyze_plan.js";
import {setParameterOnAllHosts} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

function getServerParameter(knob) {
    return assert.commandWorked(db.adminCommand({getParameter: 1, [knob]: 1}))[knob];
}

function setServerParameter(knob, value) {
    setParameterOnAllHosts(DiscoverTopology.findNonConfigNodes(db.getMongo()), knob, value);
}

function getTextOrExecutionStats(explain) {
    const result = [];
    for (let stages of getExecutionStages(explain)) {
        result.push(getPlanStage(stages, "TEXT_OR"));
    }
    return result;
}

function assertSpilledToDisk(textOrStages) {
    const totalSpillingStats = {
        usedDisk: false,
        spills: 0,
        spilledBytes: 0,
        spilledRecords: 0,
        spilledDataStorageSize: 0,
    };

    // Accumulate spilling stats across shards if the collection is sharded.
    // We can't assert individual shards, because some shards may have so little data that they
    // don't spill.
    for (let textOrStage of textOrStages) {
        totalSpillingStats.usedDisk = (totalSpillingStats.usedDisk || textOrStage.usedDisk);
        totalSpillingStats.spills += textOrStage.spills;
        totalSpillingStats.spilledBytes += textOrStage.spilledBytes;
        totalSpillingStats.spilledRecords += textOrStage.spilledRecords;
        totalSpillingStats.spilledDataStorageSize += textOrStage.spilledDataStorageSize;
    }

    assert.eq(totalSpillingStats.usedDisk, true, textOrStages);
    assert.between(1, totalSpillingStats.spills, 1000, textOrStages);
    assert.between(1, totalSpillingStats.spilledRecords, 1000, textOrStages);
    assert.between(1000, totalSpillingStats.spilledBytes, 100000, textOrStages);
    assert.between(500, totalSpillingStats.spilledDataStorageSize, 50000, textOrStages);
}

function assertDidNotSpillToDisk(textOrStages) {
    for (let textOrStage of textOrStages) {
        assert.eq(textOrStage.usedDisk, false);
        assert.eq(textOrStage.spills, 0);
        assert.eq(textOrStage.spilledBytes, 0);
        assert.eq(textOrStage.spilledRecords, 0);
        assert.eq(textOrStage.spilledDataStorageSize, 0);
    }
}

const coll = db.getSiblingDB("test").getCollection("fts_spilling");
coll.drop();

const words1 = [
    "red",
    "orange",
    "yellow",
    "green",
    "blue",
    "purple",
];
const words2 = [
    "tea",
    "coffee",
    "soda",
    "water",
    "juice",
    "fresh",
];
const words3 = [
    "drink",
    "beverage",
    "refreshment",
    "hydration",
];

let price = 0;
for (let word1 of words1) {
    for (let word2 of words2) {
        for (let word3 of words3) {
            assert.commandWorked(
                coll.insertOne({desc: word1 + " " + word2 + " " + word3, price: price}));
            price = (price + 2) % 7;
        }
    }
}
assert.commandWorked(coll.createIndex({desc: "text", price: 1}));

const predicate = {
    $text: {$search: "green tea drink"},
    price: {$lte: 5},
};
const resultWithoutSpilling = coll.find(predicate, {score: {$meta: "textScore"}})
                                  .sort({_: {$meta: "textScore"}, _id: 1})
                                  .toArray();

const explainWithoutSpilling =
    coll.find({$text: {$search: "green tea drink"}}, {score: {$meta: "textScore"}})
        .explain("executionStats");
assertDidNotSpillToDisk(getTextOrExecutionStats(explainWithoutSpilling));

const originalKnobValue = getServerParameter("internalTextOrStageMaxMemoryBytes");
try {
    setServerParameter("internalTextOrStageMaxMemoryBytes", 128);

    const result = coll.find(predicate, {score: {$meta: "textScore"}})
                       .sort({_: {$meta: "textScore"}, _id: 1})
                       .toArray();
    assert.eq(resultWithoutSpilling, result, () => arrayDiff(resultWithoutSpilling, result));

    const explain = coll.find(predicate, {score: {$meta: "textScore"}})
                        .sort({_: {$meta: "textScore"}, _id: 1})
                        .explain("executionStats");
    assertSpilledToDisk(getTextOrExecutionStats(explain));
} finally {
    setServerParameter("internalTextOrStageMaxMemoryBytes", originalKnobValue);
}
