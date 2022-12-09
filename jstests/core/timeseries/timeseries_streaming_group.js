/**
 * Tests that the $group stage will be replaced with $_internalStreamingGroup when group id is
 * monotonic on time and documents are sorted on time.
 *
 * @tags: [
 *   # Explain of a resolved view must be executed by mongos.
 *   directly_against_shardsvrs_incompatible,
 *   does_not_support_stepdowns,
 *   requires_fcv_63,
 *   requires_timeseries,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");     // For getAggPlanStages
load("jstests/libs/fail_point_util.js");  // For configureFailPoint

const ts = db.timeseries_streaming_group;
ts.drop();

const coll = db.timeseires_streaming_group_regular_collection;
coll.drop();

assert.commandWorked(
    db.createCollection(ts.getName(), {timeseries: {timeField: "time", metaField: "meta"}}));

const numTimes = 100;
const numSymbols = 10;
const minPrice = 100;
const maxPrice = 200;
const minAmount = 1;
const maxAmount = 20;

Random.setRandomSeed(1);

const randRange = function(min, max) {
    return min + Random.randInt(max - min);
};

const symbols = [];
for (let i = 0; i < numSymbols; i++) {
    let randomName = "";
    const randomStrLen = 5;
    for (let j = 0; j < randomStrLen; j++) {
        randomName += String.fromCharCode("A".charCodeAt(0) + Random.randInt(26));
    }
    symbols.push(randomName);
}

const documents = [];
const startTime = 1641027600000;
for (let i = 0; i < numTimes; i++) {
    for (const symbol of symbols) {
        documents.push({
            time: new Date(startTime + i * 1000),
            price: randRange(minPrice, maxPrice),
            amount: randRange(minAmount, maxAmount),
            meta: {"symbol": symbol}
        });
    }
}

assert.commandWorked(ts.insert(documents));
assert.commandWorked(coll.insert(documents));

const runTest = function(pipeline, expectedMonotonicIdFields) {
    const explain = assert.commandWorked(ts.explain().aggregate(pipeline));
    const streamingGroupStage = getAggPlanStage(explain, "$_internalStreamingGroup");
    assert.neq(streamingGroupStage, null);
    assert.eq(streamingGroupStage.$_internalStreamingGroup.$monotonicIdFields,
              expectedMonotonicIdFields);

    const found = ts.aggregate(pipeline).toArray();
    const expected = coll.aggregate(pipeline).toArray();
    assert.eq(expected, found);
};

runTest(
    [
        {$sort: {time: 1}},
        {
            $group: {
                _id: {
                    symbol: "$meta.symbol",
                    minute: {
                        $subtract: [
                            {$dateTrunc: {date: "$time", unit: "minute"}},
                            {$dateTrunc: {date: new Date(startTime), unit: "minute"}},
                        ]
                    }
                },
                "average_price": {$avg: {$multiply: ["$price", "$amount"]}},
                "average_amount": {$avg: "$amount"}
            }
        },
        {$addFields: {"average_price": {$divide: ["$average_price", "$average_amount"]}}},
        {$sort: {_id: 1}}
    ],
    ["minute"]);

runTest(
    [
        {$sort: {time: 1}},
        {
            $group: {
                _id: {$dateTrunc: {date: "$time", unit: "minute"}},
                "average_price": {$avg: {$multiply: ["$price", "$amount"]}},
                "average_amount": {$avg: "$amount"}
            }
        },
        {$addFields: {"average_price": {$divide: ["$average_price", "$average_amount"]}}},
        {$sort: {_id: 1}}
    ],
    ["_id"]);

runTest(
    [
        {$sort: {time: 1}},
        {
            $group: {
                _id: "$time",
                "average_price": {$avg: {$multiply: ["$price", "$amount"]}},
                "average_amount": {$avg: "$amount"}
            }
        },
        {$addFields: {"average_price": {$divide: ["$average_price", "$average_amount"]}}},
        {$sort: {_id: 1}}
    ],
    ["_id"]);
})();
