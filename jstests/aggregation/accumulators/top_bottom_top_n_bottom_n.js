/**
 * Basic tests for the $top/$bottom/$topN/$bottomN accumulators.
 */
(function() {
"use strict";

load("jstests/libs/sbe_assert_error_override.js");  // Override error-code-checking APIs.

const coll = db[jsTestName()];
coll.drop();

const largestInt =
    NumberDecimal("9223372036854775807");  // This is max int64 which is supported as N.
const largestIntPlus1 = NumberDecimal("9223372036854775808");  // Adding 1 puts it over the edge.

// Makes a string for a unique sales associate name that looks like 'Jim the 4 from CA'.
const associateName = (i, state) => ["Jim", "Pam", "Dwight", "Phyllis"][i % 4] + " the " +
    parseInt(i / 4) + " from " + state;

// Basic correctness tests.
let docs = [];
const defaultN = 4;
const states = [{state: "CA", sales: 10}, {state: "NY", sales: 7}, {state: "TX", sales: 4}];
let expectedBottomNAscResults = [];
let expectedTopNAscResults = [];
let expectedBottomNDescResults = [];
let expectedTopNDescResults = [];

for (const stateDoc of states) {
    const state = stateDoc["state"];
    const sales = stateDoc["sales"];
    let lowSales = [];
    let highSales = [];
    for (let i = 1; i <= sales; ++i) {
        const amount = i * 100;
        const associate = associateName(i, state);
        docs.push({state, sales: amount, associate});

        // Record the lowest/highest 'n' values.
        if (i < defaultN + 1) {
            lowSales.push(associate);
        }
        if (sales - defaultN < i) {
            highSales.push(associate);
        }
    }

    // The lowest values will be present in the output for topN ascending and bottomN descending.
    expectedTopNAscResults.push({_id: state, associates: lowSales});
    expectedBottomNDescResults.push({_id: state, associates: lowSales.slice().reverse()});

    // The highest values will be present in the output for bottomN ascending and topN descending.
    expectedBottomNAscResults.push({_id: state, associates: highSales});
    expectedTopNDescResults.push({_id: state, associates: highSales.slice().reverse()});
}

assert.commandWorked(coll.insert(docs));

// Helper to construct a valid $topN/$bottomN specification.
function buildTopNBottomNSpec(op, sortSpec, outputSpec, nValue) {
    return {[op]: {output: outputSpec, n: nValue, sortBy: sortSpec}};
}

/**
 * Helper that verifies that 'op' and 'sortSpec' produce 'expectedResults'.
 */
function assertExpected(op, sortSpec, expectedResults) {
    const actual =
        coll.aggregate([
                {
                    $group: {
                        _id: "$state",
                        associates: buildTopNBottomNSpec(op, sortSpec, "$associate", defaultN)
                    }
                },
                {$sort: {_id: 1}}
            ])
            .toArray();
    assert.eq(expectedResults, actual);

    // Basic correctness test for $top/$topN/$bottom/$bottomN used in $bucketAuto. Though
    // $bucketAuto uses accumulators in the same way that $group does, the test below verifies that
    // everything works properly with serialization and reporting results. Note that the $project
    // allows us to compare the $bucketAuto results to the expected $group results (because there
    // are more buckets than groups, it will always be the case that the min value of each bucket
    // corresponds to the group key).
    let actualBucketAutoResults =
        coll.aggregate([
                {
                    $bucketAuto: {
                        groupBy: '$state',
                        buckets: 10 * 1000,
                        output:
                            {associates: buildTopNBottomNSpec(op, sortSpec, "$associate", defaultN)}
                    }
                },
                {$project: {_id: "$_id.min", associates: 1}},
                {$sort: {_id: 1}},
            ])
            .toArray();

    // Using a computed projection will put the fields out of order. As such, we re-order them
    // below.
    for (let i = 0; i < actualBucketAutoResults.length; ++i) {
        const currentDoc = actualBucketAutoResults[i];
        actualBucketAutoResults[i] = {_id: currentDoc._id, associates: currentDoc.associates};
    }
    assert.eq(expectedResults, actualBucketAutoResults);
}

// Note that the output documents are sorted by '_id' so that we can compare actual groups against
// expected groups (we cannot perform unordered comparison because order matters for $topN/bottomN).
assertExpected("$bottomN", {sales: 1}, expectedBottomNAscResults);
assertExpected("$topN", {sales: 1}, expectedTopNAscResults);
assertExpected("$bottomN", {sales: -1}, expectedBottomNDescResults);
assertExpected("$topN", {sales: -1}, expectedTopNDescResults);

// Verify that we can compute multiple topN/bottomN groupings in the same $group.
const combinedGroup =
    coll.aggregate([
            {
                $group: {
                    _id: "$state",
                    bottomAsc: buildTopNBottomNSpec("$bottomN", {sales: 1}, "$associate", defaultN),
                    bottomDesc:
                        buildTopNBottomNSpec("$bottomN", {sales: -1}, "$associate", defaultN),
                    topAsc: buildTopNBottomNSpec("$topN", {sales: 1}, "$associate", defaultN),
                    topDesc: buildTopNBottomNSpec("$topN", {sales: -1}, "$associate", defaultN)
                }
            },
            {$sort: {_id: 1}}
        ])
        .toArray();

let bottomAsc = [];
let bottomDesc = [];
let topAsc = [];
let topDesc = [];
for (const doc of combinedGroup) {
    bottomAsc.push({_id: doc["_id"], associates: doc["bottomAsc"]});
    bottomDesc.push({_id: doc["_id"], associates: doc["bottomDesc"]});
    topAsc.push({_id: doc["_id"], associates: doc["topAsc"]});
    topDesc.push({_id: doc["_id"], associates: doc["topDesc"]});
}

assert.eq([bottomAsc, bottomDesc, topAsc, topDesc], [
    expectedBottomNAscResults,
    expectedBottomNDescResults,
    expectedTopNAscResults,
    expectedTopNDescResults
]);

// Verify that we can dynamically compute 'n' based on the group key for $group.
const groupKeyNExpr = {
    $cond: {if: {$eq: ["$st", "CA"]}, then: 10, else: 4}
};
const dynamicBottomNResults =
    coll.aggregate([{
            $group: {
                _id: {"st": "$state"},
                bottomAssociates:
                    {$bottomN: {output: "$associate", n: groupKeyNExpr, sortBy: {sales: 1}}}
            }
        }])
        .toArray();

// Verify that the 'CA' group has 10 results, while all others have only 4.
for (const result of dynamicBottomNResults) {
    assert(result.hasOwnProperty("_id"), tojson(result));
    const groupKey = result["_id"];
    assert(groupKey.hasOwnProperty("st"), tojson(groupKey));
    const state = groupKey["st"];
    assert(result.hasOwnProperty("bottomAssociates"), tojson(result));
    const salesArray = result["bottomAssociates"];
    if (state === "CA") {
        assert.eq(salesArray.length, 10, tojson(salesArray));
    } else {
        assert.eq(salesArray.length, 4, tojson(salesArray));
    }
}

// When output evaluates to missing for the single version, it should be promoted to null like in
// $first.
const outputMissing = coll.aggregate({
                              $group: {
                                  _id: "",
                                  bottom: {$bottom: {output: "$b", sortBy: {sales: 1}}},
                                  top: {$top: {output: "$b", sortBy: {sales: 1}}}
                              }
                          })
                          .toArray();
assert.eq(null, outputMissing[0]["top"]);
assert.eq(null, outputMissing[0]["bottom"]);

// Error cases.

// Cannot reference the group key in $bottomN when using $bucketAuto.
assert.commandFailedWithCode(coll.runCommand("aggregate", {
    pipeline: [{
        $bucketAuto: {
            groupBy: "$state",
            buckets: 2,
            output: {
                bottomAssociates:
                    {$bottomN: {output: "$associate", n: groupKeyNExpr, sortBy: {sales: 1}}}
            }
        }
    }],
    cursor: {}
}),
                             4544714);

// Verify that 'n' cannot be greater than the largest signed 64 bit int.
assert.commandFailedWithCode(coll.runCommand("aggregate", {
    pipeline: [{
        $group: {
            _id: {'st': '$state'},
            sales: {$topN: {output: "$associate", n: largestIntPlus1, sortBy: {sales: 1}}}
        }
    }],
    cursor: {}
}),
                             5787903);

assert(coll.drop());

// Verify that $topN/$bottomN respects the specified sort order.
function gameScoreGenerator(i) {
    const players = ["Mihai", "Mickey", "Kyle", "Bob", "Joe", "Rebecca", "Jane", "Jill", "Sarah"];
    const playerId = i % players.length;
    const playersPerGame = 10;
    const gameId = Math.floor(i / playersPerGame);
    const score = i % 3 === 1 ? i : 3 * i;
    return {
        _id: i,
        game: "G" + gameId,
        player: players[playerId] + Math.floor(i / players.length),
        score: score
    };
}
const nGames = 100;
let games = [];
for (let i = 0; i < nGames; ++i) {
    games.push(gameScoreGenerator(i));
}
assert.commandWorked(coll.insert(games));

const gameSpec = {
    player: "$player",
    score: "$score"
};
for (const nVal of [defaultN, largestInt]) {
    const gameResults =
        coll.aggregate([{
                $group: {
                    _id: "$game",
                    bottomAsc: buildTopNBottomNSpec("$bottomN", {score: 1}, gameSpec, nVal),
                    topAsc: buildTopNBottomNSpec("$topN", {score: 1}, gameSpec, nVal),
                    topDesc: buildTopNBottomNSpec("$topN", {score: -1}, gameSpec, nVal),
                    bottomDesc: buildTopNBottomNSpec("$bottomN", {score: -1}, gameSpec, nVal),
                }
            }])
            .toArray();

    for (const doc of gameResults) {
        let assertResultsInOrder = function(index, fieldName, arr, isAsc) {
            const [first, second] = [arr[index - 1]["score"], arr[index]["score"]];
            const cmpResult = isAsc ? first < second : first > second;
            assert(cmpResult,
                   "Incorrect order from accumulator corresponding to field '" + fieldName +
                       "'; results: " + tojson(arr));
        };

        let testFieldNames = function(fNames, isAsc) {
            for (const fieldName of fNames) {
                const arr = doc[fieldName];
                // Verify that 'nVal' is greater or equal to the number of results returned.
                // Note that we upconvert to NumberDecimal to account for 'largestInt' being a
                // NumberDecimal.
                assert.gte(NumberDecimal(nVal),
                           NumberDecimal(arr.length),
                           nVal + " is not GTE array length of " + tojson(arr) + " for field " +
                               fieldName);
                for (let i = 1; i < arr.length; ++i) {
                    assertResultsInOrder(i, fieldName, arr, isAsc);
                }
            }
        };

        testFieldNames(["bottomAsc", "topAsc"], true);
        testFieldNames(["bottomDesc", "topDesc"], false);
    }
}

const rejectInvalidSpec = (op, assign, errCode, delProps = []) => {
    let spec = Object.assign({}, {output: "$associate", n: 2, sortBy: {sales: 1}}, assign);
    delProps.forEach(delProp => delete spec[delProp]);
    assert.commandFailedWithCode(coll.runCommand("aggregate", {
        pipeline: [{$group: {_id: {"st": "$state"}, bottomAssociates: {[op]: spec}}}],
        cursor: {}
    }),
                                 errCode);
};

// Reject non-integral/negative values of n.
rejectInvalidSpec("$bottomN", {n: "string"}, 5787902);
rejectInvalidSpec("$bottomN", {n: 3.2}, 5787903);
rejectInvalidSpec("$bottomN", {n: -1}, 5787908);

// Missing arguments.
rejectInvalidSpec("$bottomN", {}, 5788003, ["n"]);
rejectInvalidSpec("$bottomN", {}, 5788004, ["output"]);
rejectInvalidSpec("$bottomN", {}, 5788005, ["sortBy"]);

// Invalid sort spec.
rejectInvalidSpec("$bottomN", {sortBy: {sales: "coffee"}}, 15974);
rejectInvalidSpec("$bottomN", {sortBy: {sales: 2}}, 15975);
rejectInvalidSpec("$bottomN", {sortBy: "sales"}, 10065);

// Extra field.
rejectInvalidSpec("$bottomN", {edgar: true}, 5788002);
// Rejects n for non-n version.
rejectInvalidSpec("$bottom", {}, 5788002);

// Sort on embedded field.
assert(coll.drop());
assert.commandWorked(coll.insertMany([4, 2, 3, 1].map((i) => ({a: {b: i}}))));
const embeddedResult =
    coll.aggregate(
            {$group: {_id: "", result: {$bottomN: {n: 3, output: "$a.b", sortBy: {"a.b": 1}}}}})
        .toArray();
assert.eq([2, 3, 4], embeddedResult[0].result);

// Compound Sorting.
coll.drop();
const as = [1, 2, 3];
const bs = [1, 2, 3];
const crossProduct = (arr1, arr2) =>
    arr1.map(a => arr2.map(b => ({a, b}))).reduce((docs, inner) => docs.concat(inner));
const fullAscending = crossProduct(as, bs);
const aAscendingBDecending = crossProduct(as, bs.reverse());

assert.commandWorked(coll.insertMany(fullAscending));
const actualFullAscending =
    coll.aggregate({
            $group: {
                _id: "",
                sorted: {$bottomN: {n: 9, output: {a: "$a", b: "$b"}, sortBy: {a: 1, b: 1}}}
            }
        })
        .toArray();
assert.eq(fullAscending, actualFullAscending[0]["sorted"]);

const actualAAscendingBDecending =
    coll.aggregate({
            $group: {
                _id: "",
                sorted: {$bottomN: {n: 9, output: {a: "$a", b: "$b"}, sortBy: {a: 1, b: -1}}}
            }
        })
        .toArray();
assert.eq(aAscendingBDecending, actualAAscendingBDecending[0]["sorted"]);

// $meta sort specification.
assert(coll.drop());
assert.commandWorked(coll.insertMany(
    ["apples apples pears", "pears pears", "apples apples apples", "apples doughnuts"].map(
        text => ({text}))));
assert.commandWorked(coll.createIndex({text: "text"}));
const sortStageResult =
    coll.aggregate(
            [{$match: {$text: {$search: "apples pears"}}}, {$sort: {text: {$meta: "textScore"}}}])
        .toArray()
        .map(doc => doc["text"]);
const testOperatorText = (op) => {
    const opNResult =
        coll.aggregate([
                {$match: {$text: {$search: "apples pears"}}},
                {
                    $group: {
                        _id: "",
                        result: {
                            [op]: {n: 4, output: "$text", sortBy: {"a.text": {$meta: "textScore"}}}
                        }
                    }
                }
            ])
            .toArray();
    assert.eq(opNResult.length, 1);
    assert.eq(sortStageResult, opNResult[0]["result"]);
};

// Note that $topN and $bottomN will return the same results because $meta sort always returns the
// most relevant results first.
testOperatorText("$bottomN");
testOperatorText("$topN");
})();
