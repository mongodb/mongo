/*
 * This jstest demonstrates simple ways to retrieve null and undefined data from a collection using
 * $type.
 * @tags: [assumes_no_implicit_index_creation]
 */

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {isCollscan, isIxscan} from "jstests/libs/analyze_plan.js";

const coll = db[jsTestName()];
coll.drop();
assert.commandWorked(coll.insert([
    {_id: 0, a: 1},
    {_id: 1, b: 1},

    {_id: 2, a: null},
    {_id: 3, a: undefined},

    {_id: 4, a: [1, 2, 3]},
    {_id: 5, a: [1, null]},
    {_id: 6, a: [null, undefined]},
    {_id: 7, a: [1, undefined]},
    {_id: 8, a: [1, null, undefined]}
]));

function runPipeline(pipeline, expected) {
    const res = coll.aggregate(pipeline).toArray();
    assertArrayEq({
        actual: res,
        expected: expected,
        extraErrorMsg: "Aggregation did not return expected results: " + tojson(pipeline)
    });
}

function runQuery(query, expected) {
    const res = coll.find(query).toArray();
    assertArrayEq({
        actual: res,
        expected: expected,
        extraErrorMsg: "Query for undefined did not return expected results: " + tojson(query)
    });
}

runQuery({a: {$type: "undefined"}}, [
    {_id: 3, a: undefined},
    {_id: 6, a: [null, undefined]},
    {_id: 7, a: [1, undefined]},
    {_id: 8, a: [1, null, undefined]}
]);

runQuery({a: {$type: ["null", "undefined"]}}, [
    {_id: 2, a: null},
    {_id: 3, a: undefined},
    {_id: 5, a: [1, null]},
    {_id: 6, a: [null, undefined]},
    {_id: 7, a: [1, undefined]},
    {_id: 8, a: [1, null, undefined]}
]);

runQuery({$or: [{a: {$eq: null}}, {a: {$type: "undefined"}}]}, [
    {_id: 1, b: 1},
    {_id: 2, a: null},
    {_id: 3, a: undefined},
    {_id: 5, a: [1, null]},
    {_id: 6, a: [null, undefined]},
    {_id: 7, a: [1, undefined]},
    {_id: 8, a: [1, null, undefined]}
]);

runQuery({$expr: {$eq: [{$type: "$a"}, "undefined"]}}, [{_id: 3, a: undefined}]);

runQuery({$expr: {$in: [{$type: "$a"}, ["null", "undefined"]]}},
         [{_id: 2, a: null}, {_id: 3, a: undefined}]);

// Shows the behavior of regular let/pipeline syntax used for field equality
runPipeline(
    [
        {
            $lookup: {
                from: coll.getName(), 
                as: "res", 
                let : {local_a: "$a"}, 
                pipeline: [ 
                    {$match: {$expr: {$eq: ["$$local_a", "$a"]}}}, 
                    {$project: {_id: 1}}
                ]
            }
        }
    ], 
    [
        // Every document matches itself.
        {_id: 0, a: 1, res: [{_id: 0}]},

        // Missing also matches undefined ($expr $eq semantics).
        {_id: 1, b: 1, res: [{_id: 1}, {_id: 3}]},

        // Null matches null ONLY.
        {_id: 2, a: null, res: [{_id: 2}]},

        // Undefined matches undefined and missing ($expr $eq semantics).
        {_id: 3, a: undefined, res: [{_id: 1}, {_id: 3}]},

        {_id: 4, a: [1, 2, 3], res: [{_id: 4}]},
        {_id: 5, a: [1, null], res: [ {_id: 5}]},
        {_id: 6, a: [null, undefined], res: [{_id: 6}]},
        {_id: 7, a: [1, undefined], res: [{_id: 7}]},
        {_id: 8, a: [1, null, undefined], res: [{_id: 8}]}
    ]
);

// Shows the behavior when we add a predicate allowing null to match undefined.
runPipeline(
    [
        {
            $lookup: {
                from: coll.getName(),
                as: "res",
                let: {local_a: "$a"},
                pipeline: [
                    {$match: {
                        $or: [
                            {$expr: {$eq: ["$$local_a", "$a"]}}, 
                            {$and: [
                                {$expr: {$eq: ["$$local_a", null]}}, 
                                {$expr: {$eq: [{$type: "$a"}, "undefined"]}}
                            ]}
                        ]
                    }}, 
                    {$project: {_id: 1}}
                ]
            }
        }
    ], 
    [
        // Every document matches itself.
        {_id: 0, a: 1, res: [{_id: 0}]},

        // Missing also matches undefined ($expr $eq semantics).
        {_id: 1, b: 1, res: [{_id: 1}, {_id: 3}]},

        // Null matches null and undefined.
        {_id: 2, a: null, res: [{_id: 2}, {_id: 3}]},

        // Undefined matches undefined and missing ($expr $eq semantics).
        {_id: 3, a: undefined, res: [{_id: 1}, {_id: 3}]},

        {_id: 4, a: [1, 2, 3], res: [{_id: 4}]},
        {_id: 5, a: [1, null], res: [ {_id: 5}]},
        {_id: 6, a: [null, undefined], res: [{_id: 6}]},
        {_id: 7, a: [1, undefined], res: [{_id: 7}]},
        {_id: 8, a: [1, null, undefined], res: [{_id: 8}]}
    ]
);

// Show interactions with indexes.
const typeUndefined = {
    a: {$type: "undefined"}
};
const expectedRes = [
    {_id: 3, a: undefined},
    {_id: 6, a: [null, undefined]},
    {_id: 7, a: [1, undefined]},
    {_id: 8, a: [1, null, undefined]}
];

// {$type: undefined} can be satisfied with an index scan.
assert.commandWorked(coll.createIndex({a: 1}));
runQuery(typeUndefined, expectedRes);
let explain = coll.find(typeUndefined).explain();
assert(isIxscan(db, explain),
       "Expected $type query to be able to use index, but it did not: " + tojson(explain));

// A partial filter index with {a: $eq: null} cannot be used for {a: {$type: undefined}}
assert.commandWorked(coll.dropIndexes());
assert.commandWorked(coll.createIndex({a: 1}, {partialFilterExpression: {a: {$eq: null}}}));
runQuery(typeUndefined, expectedRes);
explain = coll.find(typeUndefined).explain();
assert(isCollscan(db, explain),
       "Expected $type query to use collection scan, but it did not: " + tojson(explain));
