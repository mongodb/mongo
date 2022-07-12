/**
 * Testing of just the query layer's integration for columnar index.
 * @tags: [
 *   # columnstore indexes are new in 6.1.
 *   requires_fcv_61,
 *   # Runs explain on an aggregate command which is only compatible with readConcern local.
 *   assumes_read_concern_unchanged,
 *   # We could potentially need to resume an index build in the event of a stepdown, which is not
 *   # yet implemented.
 *   does_not_support_stepdowns,
 *   # Columnstore indexes are incompatible with clustered collections.
 *   incompatible_with_clustered_collection,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/analyze_plan.js");         // For "planHasStage."
load("jstests/aggregation/extras/utils.js");  // For "resultsEq."
load("jstests/libs/sbe_util.js");             // For "checkSBEEnabled.""

const columnstoreEnabled =
    checkSBEEnabled(db, ["featureFlagColumnstoreIndexes", "featureFlagSbeFull"]);
if (!columnstoreEnabled) {
    jsTestLog("Skipping columnstore index validation test since the feature flag is not enabled.");
    return;
}

const coll = db.columnstore_index_correctness;
coll.drop();

// Intentionally not using _id as the unique identifier, to avoid getting IDHACK plans when we
// query by it.
const docs = [
    {num: 0},
    {num: 1, a: null},
    {num: 2, a: "scalar"},
    {num: 3, a: {}},
    {num: 4, a: {x: 1, b: "scalar"}},
    {num: 5, a: {b: {}}},
    {num: 6, a: {x: 1, b: {}}},
    {num: 7, a: {x: 1, b: {x: 1}}},
    {num: 8, a: {b: {c: "scalar"}}},
    {num: 9, a: {b: {c: null}}},
    {num: 10, a: {b: {c: [[1, 2], [{}], 2]}}},
    {num: 11, a: {x: 1, b: {x: 1, c: ["scalar"]}}},
    {num: 12, a: {x: 1, b: {c: {x: 1}}}},
    {num: 13, a: {b: []}},
    {num: 14, a: {b: [null]}},
    {num: 15, a: {b: ["scalar"]}},
    {num: 16, a: {b: [[]]}},
    {num: 17, a: {b: [1, {}, 2]}},
    {num: 18, a: {b: [[1, 2], [{}], 2]}},
    {num: 19, a: {x: 1, b: [[1, 2], [{}], 2]}},
    {num: 20, a: {b: [{c: "scalar"}]}},
    {num: 21, a: {b: [{c: "scalar"}, {c: "scalar2"}]}},
    {num: 22, a: {b: [{c: [[1, 2], [{}], 2]}]}},
    {num: 23, a: {b: [1, {c: "scalar"}, 2]}},
    {num: 24, a: {b: [1, {c: [[1, 2], [{}], 2]}, 2]}},
    {num: 25, a: {x: 1, b: [1, {c: [[1, 2], [{}], 2]}, 2]}},
    {num: 26, a: {b: [[1, 2], [{c: "scalar"}], 2]}},
    {num: 27, a: {b: [[1, 2], [{c: [[1, 2], [{}], 2]}], 2]}},
    {num: 28, a: {x: 1, b: [[1, 2], [{c: [[1, 2], [{}], 2]}], 2]}},
    {num: 29, a: []},
    {num: 30, a: [null]},
    {num: 31, a: ["scalar"]},
    {num: 32, a: [[]]},
    {num: 33, a: [{}]},
    {num: 34, a: [1, {}, 2]},
    {num: 35, a: [[1, 2], [{}], 2]},
    {num: 36, a: [{b: "scalar"}]},
    {num: 37, a: [{b: null}]},
    {num: 38, a: [1, {b: "scalar"}, 2]},
    {num: 39, a: [1, {b: []}, 2]},
    {num: 40, a: [1, {b: [null]}, 2]},
    {num: 41, a: [1, {b: ["scalar"]}, 2]},
    {num: 42, a: [1, {b: [[]]}, 2]},
    {num: 43, a: [{b: []}]},
    {num: 44, a: [{b: ["scalar"]}]},
    {num: 45, a: [{b: [[]]}]},
    {num: 46, a: [{b: {}}]},
    {num: 47, a: [{b: {c: "scalar"}}]},
    {num: 48, a: [{b: {c: [[1, 2], [{}], 2]}}]},
    {num: 49, a: [{b: {x: 1}}]},
    {num: 50, a: [{b: {x: 1, c: "scalar"}}]},
    {num: 51, a: [{b: [{c: "scalar"}]}]},
    {num: 52, a: [{b: [{c: ["scalar"]}]}]},
    {num: 53, a: [{b: [1, {c: ["scalar"]}, 2]}]},
    {num: 54, a: [{b: [{}]}]},
    {num: 55, a: [{b: [[1, 2], [{}], 2]}]},
    {num: 56, a: [{b: [[1, 2], [{c: "scalar"}], 2]}]},
    {num: 57, a: [{b: [[1, 2], [{c: ["scalar"]}], 2]}]},
    {num: 58, a: [1, {b: {}}, 2]},
    {num: 59, a: [1, {b: {c: "scalar"}}, 2]},
    {num: 60, a: [1, {b: {c: {x: 1}}}, 2]},
    {num: 61, a: [1, {b: {c: [1, {}, 2]}}, 2]},
    {num: 62, a: [1, {b: {x: 1}}, 2]},
    {num: 63, a: [1, {b: {x: 1, c: "scalar"}}, 2]},
    {num: 64, a: [1, {b: {x: 1, c: [[]]}}, 2]},
    {num: 65, a: [1, {b: {x: 1, c: [1, {}, 2]}}, 2]},
    {num: 66, a: [1, {b: [{}]}, 2]},
    {num: 67, a: [1, {b: [{c: "scalar"}]}, 2]},
    {num: 68, a: [1, {b: [{c: {x: 1}}]}, 2]},
    {num: 69, a: [1, {b: [{c: [1, {}, 2]}]}, 2]},
    {num: 70, a: [1, {b: [1, {}, 2]}, 2]},
    {num: 71, a: [1, {b: [1, {c: null}, 2]}, 2]},
    {num: 72, a: [1, {b: [1, {c: "scalar"}, 2]}, 2]},
    {num: 73, a: [1, {b: [1, {c: [1, {}, 2]}, 2]}, 2]},
    {num: 74, a: [1, {b: [[1, 2], [{}], 2]}, 2]},
    {num: 75, a: [1, {b: [[1, 2], [{c: "scalar"}], 2]}, 2]},
    {num: 76, a: [1, {b: [[1, 2], [{c: [1, {}, 2]}], 2]}, 2]},
    {num: 77, a: [[1, 2], [{b: "scalar"}], 2]},
    {num: 78, a: [[1, 2], [{b: {x: 1, c: "scalar"}}], 2]},
    {num: 79, a: [[1, 2], [{b: {x: 1, c: [1, {}, 2]}}], 2]},
    {num: 80, a: [[1, 2], [{b: []}], 2]},
    {num: 81, a: [[1, 2], [{b: [1, {c: "scalar"}, 2]}], 2]},
    {num: 82, a: [[1, 2], [{b: [[1, 2], [{c: "scalar"}], 2]}], 2]},
    {num: 83, a: [[1, 2], [{b: [[1, 2], [{c: [[1, 2], [{}], 2]}], 2]}], 2]},
    {num: 84, a: [{b: [{c: 1}, {}]}]},
    {num: 85, a: [{b: [{c: 1}, {d: 1}]}]},
    {num: 86, a: [{b: {c: 1}}, {b: {}}]},
    {num: 87, a: [{b: {c: 1}}, {b: {d: 1}}]},
    {num: 88, a: [{b: {c: 1}}, {}]},
    {num: 89, a: [{b: {c: 1}}, {b: null}]},
    {num: 90, a: [{b: {c: 1}}, {b: []}]},
    {num: 91, a: [{b: []}, {b: []}]},
    {num: 92, a: {b: [{c: [1, 2]}]}},
    {num: 93, a: {b: {c: [1, 2]}}},
    {num: 94, a: [[1, 2], [{b: [[1, 2], [{c: [[1, 2], [{}], 2]}], 2]}], 2]},
    {num: 95, a: [{m: 1, n: 2}, {m: 2, o: 1}]},
];

let docNum = 0;
let bulk = coll.initializeUnorderedBulkOp();
for (let doc of docs) {
    let numObj = {num: docNum++};
    let insertObj = {};
    Object.assign(insertObj, numObj, doc);
    if (docNum % 2 == 0) {
        insertObj.optionalField = "foo";
    }
    bulk.insert(insertObj);
}
bulk.execute();

assert.commandWorked(coll.createIndex({"$**": "columnstore"}));
const kProjection = {
    _id: 0,
    "a.b.c": 1,
    num: 1,
    optionalField: 1
};

// Run an explain.
let explain = coll.find({}, kProjection).explain();
assert(planHasStage(db, explain, "COLUMN_SCAN"), explain);

// Run a query getting all of the results using the column index.
let results = coll.find({}, kProjection).toArray();
assert.gt(results.length, 0);

for (let res of results) {
    const trueResult = coll.find({num: res.num}, kProjection).hint({$natural: 1}).toArray();
    const originalDoc = coll.findOne({num: res.num});
    assert(resultsEq([res], trueResult),
           () => `column store index output ${tojson(res)}, collection scan output ${
               tojson(trueResult[0])}, original document was: ${tojson(originalDoc)}`);
}

// Run a similar query that projects multiple fields with a shared parent object.
const kSiblingProjection = {
    _id: 0,
    "a.m": 1,
    "a.n": 1,
    num: 1
};

explain = coll.find({}, kSiblingProjection).explain();
assert(planHasStage(db, explain, "COLUMN_SCAN"), explain);

results = coll.find({}, kSiblingProjection).toArray();
assert.gt(results.length, 0);
for (let res of results) {
    const trueResult =
        coll.find({num: res.num}, kSiblingProjection).hint({$natural: 1}).toArray()[0];
    const originalDoc = coll.findOne({num: res.num});
    assert.eq(res, trueResult, originalDoc);
}

// Run a query that tests the SERVER-67742 fix
const kPrefixProjection = {
    _id: 0,
    "a": 1,
    num: 1
};

// TODO SERVER-62985: Add a hint to this query to ensure it uses the column store index.
results = coll.find({"a.m": 1}, kPrefixProjection).toArray();
assert.gt(results.length, 0);
for (let res of results) {
    const trueResult =
        coll.find({num: res.num}, kPrefixProjection).hint({$natural: 1}).toArray()[0];
    const originalDoc = coll.findOne({num: res.num});
    assert.eq(res, trueResult, originalDoc);
}

// Now test grouping semantics.

// Sanity check that we are comparing the plans we expect to be.
let pipeline = [
    {$group: {_id: "$a.b.c", docs: {$push: "$num"}}},
    {$set: {docs: {$sortArray: {input: "$docs", sortBy: 1}}}}
];
let naturalExplain = coll.explain().aggregate(pipeline, {hint: {$natural: 1}});
assert(aggPlanHasStage(naturalExplain, "COLLSCAN"), naturalExplain);

let nonHintedExplain = coll.explain().aggregate(pipeline);
assert(aggPlanHasStage(nonHintedExplain, "COLUMN_SCAN"), nonHintedExplain);
assert(!aggPlanHasStage(nonHintedExplain, "PROJECTION_DEFAULT"), nonHintedExplain);
assert(!aggPlanHasStage(nonHintedExplain, "PROJECTION_SIMPLE"), nonHintedExplain);

assert(resultsEq(coll.aggregate(pipeline, {hint: {$natural: 1}}).toArray(),
                 coll.aggregate(pipeline).toArray()),
       () => {
           print(`Results mismatch for $group query. Running resultsEq with verbose`);
           resultsEq(expectedResults, coll.aggregate(pipeline).toArray(), true);
       });

// For readers who are taking on the massachistic task of trying to
// verify that these results are in fact expected, the major expectations are that all arrays are
// traversed and output as the "structure" EXCEPT if there's a doubly nested array without any
// intervening path as in {a: [[{b: {c: 1}}]]}.
const expectedResults = [
    {_id: "scalar", docs: [8]},
    {_id: ["scalar", "scalar2"], docs: [21]},
    {_id: ["scalar"], docs: [11, 20, 23, 47, 50, 59, 63]},
    {_id: [1, 2], docs: [93]},
    {_id: [1, []], docs: [90]},
    {_id: [1], docs: [86, 87, 88, 89]},
    {_id: [["scalar"]], docs: [51, 67, 72]},
    {_id: [[1, 2], [{}], 2], docs: [10]},
    {_id: [[1, 2]], docs: [92]},
    {_id: [[1, {}, 2]], docs: [61, 65]},
    {_id: [[1]], docs: [84, 85]},
    {_id: [[["scalar"]]], docs: [52, 53]},
    {_id: [[[1, 2], [{}], 2]], docs: [22, 24, 25, 48]},
    {_id: [[[1, {}, 2]]], docs: [69, 73]},
    {_id: [[[]]], docs: [64]},
    {_id: [[], []], docs: [91]},
    // Note "$a.b.c" does not descend into double (directly nested) arrays as in 42,45. Might have
    // expected [[[]]]. Similarly in 56,57, it does not find the "c" values "hidden" within a
    // directly-nested array.
    {_id: [[]], docs: [39, 40, 41, 42, 43, 44, 45, 54, 55, 56, 57, 66, 70, 74, 75, 76]},
    {_id: [[null]], docs: [71]},
    {_id: [[{x: 1}]], docs: [68]},
    {
        _id: [],
        docs: [
            13, 14, 15, 16, 17, 18, 19, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35,
            36, 37, 38, 46, 49, 58, 62, 77, 78, 79, 80, 81, 82, 83, 94, 95
        ]
    },
    {_id: [{x: 1}], docs: [60]},
    {_id: null, docs: [0, 1, 2, 3, 4, 5, 6, 7, 9]},
    {_id: {x: 1}, docs: [12]},
];

assert(resultsEq(expectedResults, coll.aggregate(pipeline).toArray()), () => {
    print(`Results mismatch for $group query. Actual results: ${
        tojson(coll.aggregate(pipeline).toArray())} Running resultsEq with verbose`);
    resultsEq(expectedResults, coll.aggregate(pipeline).toArray(), true);
});
})();
