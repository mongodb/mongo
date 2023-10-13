/**
 * Test the rank based window functions.
 */
import {assertErrCodeAndErrMsgContains, documentEq} from "jstests/aggregation/extras/utils.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

const coll = db[jsTestName()];
const numDoc = 12;
for (let i = 0; i < numDoc; i++) {
    const doc = {_id: i, double: Math.floor(i / 2)};
    if (i < numDoc / 3 || i >= numDoc / 3 * 2) {
        doc.nullOrMissing = null;
    }
    coll.insert(doc);
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

// This test validates the error message displays $rank correctly.
let pipeline = [{
    $setWindowFields: {
        sortBy: {_id: 1},
        output: {rank: {$rank: "$_id"}},
    }
}]
assertErrCodeAndErrMsgContains(coll, pipeline, 5371603, "$rank");

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

// Check results with null or missing fields. Ignore this test on a sharded collection, because
// null and missing are treated the same during sorting, so the interleaving of null and missing
// is unknown after merging shard streams. Hence the rank result is also unknown.
// Ignore this test for replica sets as well, because in secondary read suites, null and missing
// might be ordered different on disk due to different record id orders.
if (!FixtureHelpers.isSharded(coll) && !FixtureHelpers.isReplSet(db)) {
    result = runRankBasedAccumulator({nullOrMissing: 1}, {$rank: {}});
    verifyResults(result, function(num, baseObj) {
        if (num < numDoc / 3) {
            baseObj.rank = 1;
        } else if (num < numDoc / 3 * 2) {
            baseObj.rank = numDoc / 3 + 1;
        } else {
            baseObj.rank = numDoc / 3 * 2 + 1;
        }
        return baseObj;
    });
    result = runRankBasedAccumulator({nullOrMissing: 1}, {$denseRank: {}});
    verifyResults(result, function(num, baseObj) {
        if (num < numDoc / 3) {
            baseObj.rank = 1;
        } else if (num < numDoc / 3 * 2) {
            baseObj.rank = 2;
        } else {
            baseObj.rank = 3;
        }
        return baseObj;
    });
}
