/**
 * Tests counters for $group and $setWindowFields accumulator expressions.
 */

(function() {
"use strict";
const mongod = MongoRunner.runMongod();
const db = mongod.getDB(jsTest.name());
const collName = jsTest.name();
const coll = db[collName];
coll.drop();

function runGroupFunction({accFn: grpFn, arg}) {
    assert(grpFn);
    arg = arg || "$b";
    const grpStage = {$group: {_id: "$a", aggResult: {[grpFn]: arg}}};
    return coll.runCommand({aggregate: coll.getName(), pipeline: [grpStage], cursor: {}});
}

function runWindowFunction({accFn: winFn, arg, skipWindow}) {
    assert(winFn);
    arg = arg || "$c";
    var winStage = null;
    if (skipWindow == true) {
        winStage = {
            $setWindowFields:
                {partitionBy: "$a", sortBy: {b: 1}, output: {accumulated: {[winFn]: arg}}}
        };
    } else {
        winStage = {
            $setWindowFields: {
                partitionBy: "$a",
                sortBy: {b: 1},
                output:
                    {accumulated: {[winFn]: arg, window: {documents: ["unbounded", "unbounded"]}}}
            }
        };
    }
    return coll.runCommand({aggregate: coll.getName(), pipeline: [winStage], cursor: {}});
}

function checkCounter(accInfo, counterType) {
    var runFn = null;
    if (counterType == "groupAccumulators") {
        runFn = runGroupFunction;
    } else if (counterType == "windowAccumulators") {
        runFn = runWindowFunction;
    } else {
        assert(false);
    }

    const beforeCounter = db.serverStatus().metrics.operatorCounters[counterType][accInfo.accFn];
    runFn(accInfo);
    const afterCounter = db.serverStatus().metrics.operatorCounters[counterType][accInfo.accFn];

    assert.eq(beforeCounter, afterCounter - 1, accInfo.accFn);
}

const groupAccumulators = [
    //'$_internalJsReduce',
    //'$accumulator',
    {accFn: '$addToSet'},
    {accFn: '$avg'},
    {accFn: '$bottom', arg: {output: "$x", sortBy: {"y": 1}}},
    {accFn: '$bottomN', arg: {n: 42, output: "$x", sortBy: {"y": 1}}},
    {accFn: '$count', arg: {}},
    {accFn: '$first'},
    {accFn: '$firstN', arg: {input: "$x", n: 42}},
    {accFn: '$last'},
    {accFn: '$lastN', arg: {input: "$x", n: 42}},
    {accFn: '$max'},
    {accFn: '$maxN', arg: {input: "$x", n: 42}},
    {accFn: '$mergeObjects'},
    {accFn: '$min'},
    {accFn: '$minN', arg: {input: "$x", n: 42}},
    {accFn: '$push'},
    {accFn: '$stdDevPop'},
    {accFn: '$stdDevSamp'},
    {accFn: '$sum'},
    {accFn: '$top', arg: {output: ["$x", "$y"], sortBy: {"b": 1}}},
    {accFn: '$topN', arg: {n: 42, output: ["$x", "$y"], sortBy: {"b": 1}}}
];

const windowAccumulators = [
    {accFn: '$addToSet'},
    {accFn: '$avg'},
    {accFn: '$bottom', arg: {output: "$x", sortBy: {"y": 1}}},
    {accFn: '$bottomN', arg: {n: 42, output: "$x", sortBy: {"y": 1}}},
    {accFn: '$count', arg: {}},
    {accFn: '$covariancePop', arg: ["$x", "$y"]},
    {accFn: '$covarianceSamp', arg: ["$x", "$y"]},
    {accFn: '$denseRank', arg: {}, skipWindow: true},
    {accFn: '$derivative', arg: {input: "$x", unit: "second"}},
    {accFn: '$documentNumber', arg: {}, skipWindow: true},
    {accFn: '$expMovingAvg', arg: {input: "$x", N: 42}, skipWindow: true},
    {accFn: '$first'},
    {accFn: '$firstN', arg: {input: "$x", n: 42}},
    {accFn: '$integral', arg: {input: "$x", unit: "hour"}},
    {accFn: '$last'},
    {accFn: '$lastN', arg: {input: "$x", n: 42}},
    {accFn: '$linearFill', skipWindow: true},  // not documented anywhere - >=5.2 ?
    {accFn: '$locf', skipWindow: true},        // not documented anywhere - >=5.2 ?
    {accFn: '$max'},
    {accFn: '$maxN', arg: {input: "$x", n: 42}},
    {accFn: '$min'},
    {accFn: '$minN', arg: {input: "$x", n: 42}},
    {accFn: '$push'},
    {accFn: '$rank', arg: {}, skipWindow: true},
    {accFn: '$shift', arg: {output: "$x", by: 1, default: "Not available"}, skipWindow: true},
    {accFn: '$stdDevPop'},
    {accFn: '$stdDevSamp'},
    {accFn: '$sum'},
    {accFn: '$top', arg: {output: ["$x", "$y"], sortBy: {"b": 1}}},
    {accFn: '$topN', arg: {n: 42, output: ["$x", "$y"], sortBy: {"b": 1}}}
];

for (const gAccInfo of groupAccumulators) {
    checkCounter(gAccInfo, "groupAccumulators");
}

for (const wAccInfo of windowAccumulators) {
    checkCounter(wAccInfo, "windowAccumulators");
}

MongoRunner.stopMongod(mongod);
})();
