/**
 * Test the rank based window functions.
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // documentEq

const coll = db[jsTestName()];
for (let i = 0; i < 12; i++) {
    coll.insert({_id: i, double: Math.floor(i / 2)});
}

let origDocs = coll.find().sort({_id: 1});
function verifyResults(results, valueFunction) {
    for (let i = 0; i < results.length; i++) {
        const correctDoc = valueFunction(i, Object.assign({}, origDocs[i]));
        assert(documentEq(correctDoc, results[i]),
               "Got: " + tojson(results[i]) + "\nExpected: " + tojson(correctDoc) +
                   "\n at position " + i + "\n");
    }
}

function runRankBasedAccumulator(sortByField, exprField) {
    return coll
        .aggregate([{
            $setWindowFields: {sortBy: sortByField, output: {rank: exprField}},
        }])
        .toArray();
}

// Rank based accumulators don't take windows.
assert.commandFailedWithCode(coll.runCommand({
    aggregate: coll.getName(),
    pipeline: [{
        $setWindowFields: {
            sortBy: {_id: 1},
            output: {rank: {$rank: {}, window: []}},
        }
    }],
    cursor: {}
}),
                             5371601);

// Rank based accumulators don't take arguments.
assert.commandFailedWithCode(coll.runCommand({
    aggregate: coll.getName(),
    pipeline: [{
        $setWindowFields: {
            sortBy: {_id: 1},
            output: {rank: {$rank: "$_id"}},
        }
    }],
    cursor: {}
}),
                             5371603);

// Rank based accumulators must have a sortBy.
assert.commandFailedWithCode(coll.runCommand({
    aggregate: coll.getName(),
    pipeline: [{
        $setWindowFields: {
            output: {rank: {$rank: {}}},
        }
    }],
    cursor: {}
}),
                             5371602);

// Rank based accumulators don't take windows.
assert.commandFailedWithCode(coll.runCommand({
    aggregate: coll.getName(),
    pipeline: [{
        $setWindowFields: {
            sortBy: {_id: 1},
            output: {rank: {$rank: {}, window: {documents: [-1, 1]}}},
        }
    }],
    cursor: {}
}),
                             5371601);

// Check results with no ties
let result = runRankBasedAccumulator({_id: 1}, {$rank: {}});
function noTieFunc(num, baseObj) {
    baseObj.rank = num + 1;
    return baseObj;
}
verifyResults(result, noTieFunc);
result = runRankBasedAccumulator({_id: 1}, {$denseRank: {}});
verifyResults(result, noTieFunc);
result = runRankBasedAccumulator({_id: 1}, {$documentNumber: {}});
verifyResults(result, noTieFunc);

// Check results with ties
origDocs = coll.find().sort({double: 1});
result = runRankBasedAccumulator({double: 1}, {$rank: {}});
verifyResults(result, function(num, baseObj) {
    if (num % 2 == 0) {
        baseObj.rank = num + 1;
    } else {
        baseObj.rank = num;
    }
    return baseObj;
});
result = runRankBasedAccumulator({double: 1}, {$denseRank: {}});
verifyResults(result, function(num, baseObj) {
    baseObj.rank = Math.floor(num / 2) + 1;
    return baseObj;
});
result = runRankBasedAccumulator({double: 1}, {$documentNumber: {}});
verifyResults(result, noTieFunc);
})();
