/**
 * Basic tests for the $top/$bottom/$topN/$bottomN accumulators.
 */
(function() {
"use strict";

const coll = db[jsTestName()];
coll.drop();

const isExactTopNEnabled = db.adminCommand({getParameter: 1, featureFlagExactTopNAccumulator: 1})
                               .featureFlagExactTopNAccumulator.value;

if (!isExactTopNEnabled) {
    // Verify that $top/$bottom/$topN/$bottomN cannot be used if the feature flag is set to false
    // and ignore the rest of the test.
    assert.commandFailedWithCode(coll.runCommand("aggregate", {
        pipeline: [{
            $group: {
                _id: {"st": "$state"},
                minSales: {$topN: {output: "$sales", n: 2, sortBy: {sales: 1}}}
            }
        }],
        cursor: {}
    }),
                                 15952);
    return;
}

// Makes a string for a unique sales associate name that looks like 'Jim the 4 from CA'.
const associateName = (i, state) => ["Jim", "Pam", "Dwight", "Phyllis"][i % 4] + " the " +
    parseInt(i / 4) + " from " + state;

// Basic correctness tests.
let docs = [];
const n = 4;
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
        if (i < n + 1) {
            lowSales.push(associate);
        }
        if (sales - n < i) {
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

/**
 * Helper that verifies that 'op' and 'sortSpec' produce 'expectedResults'.
 */
function assertExpected(op, sortSpec, expectedResults) {
    const actual =
        coll.aggregate([
                {
                    $group: {
                        _id: "$state",
                        associates: {[op]: {output: "$associate", n: n, sortBy: sortSpec}}
                    }
                },
                {$sort: {_id: 1}}
            ])
            .toArray();
    assert.eq(expectedResults, actual);
}

// Note that the output documents are sorted by '_id' so that we can compare actual groups against
// expected groups (we cannot perform unordered comparison because order matters for $topN/bottomN).
assertExpected("$bottomN", {sales: 1}, expectedBottomNAscResults);
assertExpected("$topN", {sales: 1}, expectedTopNAscResults);
assertExpected("$bottomN", {sales: -1}, expectedBottomNDescResults);
assertExpected("$topN", {sales: -1}, expectedTopNDescResults);

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
