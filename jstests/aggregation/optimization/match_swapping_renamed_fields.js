/**
 * Tests for the $match swapping optimization's ability to handle fields renamed in $project or
 * $addFields.
 * @tags: [
 *   do_not_wrap_aggregations_in_facets,
 *   requires_pipeline_optimization,
 * ]
 */
import {getAggPlanStage, getAggPlanStages} from "jstests/libs/query/analyze_plan.js";

/**
 * Utillity for inhibiting aggregate stage pushdowns to find-land queries. It doesn't prohibit other
 * pipeline optimisation techniques such as stage reordering & renames from being applied.
 */
function pipelineWithoutOptimizations(pipeline) {
    return [{$_internalInhibitOptimization: {}}, ...pipeline];
}

let coll = db.match_swapping_renamed_fields;
coll.drop();

assert.commandWorked(coll.insert([{a: 1, b: 1, c: 1}, {a: 2, b: 2, c: 2}, {a: 3, b: 3, c: 3}]));
assert.commandWorked(coll.createIndex({a: 1}));

// Test that a $match can result in index usage after moving past a field renamed by $project.
let pipeline = [{$project: {_id: 0, z: "$a", c: 1}}, {$match: {z: {$gt: 1}}}];
assert.eq(2, coll.aggregate(pipeline).itcount());
let explain = coll.explain().aggregate(pipeline);
assert.neq(null, getAggPlanStage(explain, "IXSCAN"), tojson(explain));

// Test that a $match can result in index usage after moving past a field renamed by $addFields.
pipeline = [{$addFields: {z: "$a"}}, {$match: {z: {$gt: 1}}}];
assert.eq(2, coll.aggregate(pipeline).itcount());
explain = coll.explain().aggregate(pipeline);
assert.neq(null, getAggPlanStage(explain, "IXSCAN"), tojson(explain));

// Test that a $match with $type can result in index usage after moving past a field renamed by
// $project.
pipeline = [{$project: {_id: 0, z: "$a", c: 1}}, {$match: {z: {$type: "number"}}}];
assert.eq(3, coll.aggregate(pipeline).itcount());
explain = coll.explain().aggregate(pipeline);
assert.neq(null, getAggPlanStage(explain, "IXSCAN"), tojson(explain));

// Test that a partially dependent match can split, with a rename applied, resulting in index
// usage.
pipeline = [{$project: {z: "$a", zz: {$sum: ["$a", "$b"]}}}, {$match: {z: {$gt: 1}, zz: {$lt: 5}}}];
assert.eq(1, coll.aggregate(pipeline).itcount());
explain = coll.explain().aggregate(pipeline);
assert.neq(null, getAggPlanStage(explain, "IXSCAN"), tojson(explain));

// Test that a match can swap past several renames, resulting in index usage.
pipeline = [
    {$project: {d: "$a"}},
    {$addFields: {e: "$$CURRENT.d"}},
    {$project: {f: "$$ROOT.e"}},
    {$match: {f: {$gt: 1}}}
];
assert.eq(2, coll.aggregate(pipeline).itcount());
explain = coll.explain().aggregate(pipeline);
assert.neq(null, getAggPlanStage(explain, "IXSCAN"), tojson(explain));

coll.drop();
assert.commandWorked(coll.insert({_id: 0, a: [[1]]}));
assert.commandWorked(coll.createIndex({"a": 1}));

// Test that we do NOT swap a $match before a $project with $map that puts the values of a
// non-dotted array path into a new non-dotted path.
pipeline =
    [{$project: {x: {$map: {input: "$a", as: 'iter', in : "$$iter"}}}}, {$match: {"x": [1]}}];
assert.sameMembers([{_id: 0, x: [[1]]}], coll.aggregate(pipeline).toArray());
explain = coll.explain().aggregate(pipeline);
let collscan = getAggPlanStage(explain, "COLLSCAN");
assert.neq(null, collscan, tojson(explain));

// Test that we do NOT swap a $match before a $project with $map that puts the values of a
// non-dotted array path into a new dotted path.
pipeline = [
    {$project: {x: {$map: {input: "$a", as: 'iter', in : {y: "$$iter"}}}}},
    {$match: {"x.y": [1]}}
];
assert.sameMembers([{_id: 0, x: [{y: [1]}]}], coll.aggregate(pipeline).toArray());
explain = coll.explain().aggregate(pipeline);
collscan = getAggPlanStage(explain, "COLLSCAN");
assert.neq(null, collscan, tojson(explain));

coll.drop();
assert.commandWorked(coll.insert({_id: 0, a: [{b: 1, c: 1}, {b: 2, c: 2}]}));
assert.commandWorked(coll.insert({_id: 1, a: [{b: 3, c: 3}, {b: 4, c: 4}]}));
assert.commandWorked(coll.insert({_id: 2, a: [[{b: 1, c: 2}]]}));  // doubly nested array
assert.commandWorked(coll.createIndex({"a.b": 1, "a.c": 1}));

// Test that a $match does NOT move before a $project with $map that extracts sub-fields from an
// array path. The $map is not considered a rename because it can change the shape of a document
// when the document contains arrays of arrays.
pipeline = [
    {$project: {d: {$map: {input: "$a", as: "iter", in : {e: "$$iter.b", f: "$$iter.c"}}}}},
    {$match: {"d.e": 1, "d.f": 2}}
];
assert.sameMembers([{_id: 0, d: [{e: 1, f: 1}, {e: 2, f: 2}]}, {_id: 2, d: [{e: [1], f: [2]}]}],
                   coll.aggregate(pipeline).toArray());
explain = coll.explain().aggregate(pipeline);
collscan = getAggPlanStage(explain, "COLLSCAN");
assert.neq(null, collscan, tojson(explain));

// Test that a $match does NOT fully move before a $addFields with $map that extracts sub-fields
// from an array path. This time the match expression is partially dependent and should get split.
// The $map is not considered a rename because it can change the shape of a document when the
// document contains arrays of arrays.
pipeline = [
    {$addFields: {d: {$map: {input: "$a", as: "iter", in : {e: "$$iter.b", f: "$$iter.c"}}}, g: 2}},
    {$match: {"d.e": 1, g: 2}}
];
assert.sameMembers(
    [
        {_id: 0, a: [{b: 1, c: 1}, {b: 2, c: 2}], d: [{e: 1, f: 1}, {e: 2, f: 2}], g: 2},
        {_id: 2, a: [[{b: 1, c: 2}]], d: [{e: [1], f: [2]}], g: 2}
    ],
    coll.aggregate(pipeline).toArray());
explain = coll.explain().aggregate(pipeline);
collscan = getAggPlanStage(explain, "COLLSCAN");
assert.neq(null, collscan, tojson(explain));

// Test that a $match does NOT move before a $addFields with $map that extracts sub-fields from an
// array path & computes a new field. The $map is not considered a rename because it can change the
// shape of a document when the document contains arrays of arrays.
pipeline = [
    {$addFields: {d: {$map: {input: "$a", as: "iter", in : {e: "$$iter.b", f: {$literal: 99}}}}}},
    {$match: {"d.e": 1, "d.f": 99}}
];
assert.sameMembers(
    [
        {_id: 0, a: [{b: 1, c: 1}, {b: 2, c: 2}], d: [{e: 1, f: 99}, {e: 2, f: 99}]},
        {_id: 2, a: [[{b: 1, c: 2}]], d: [{e: [1], f: 99}]}
    ],
    coll.aggregate(pipeline).toArray());
explain = coll.explain().aggregate(pipeline);
collscan = getAggPlanStage(explain, "COLLSCAN");
assert.neq(null, collscan, tojson(explain));

coll.drop();
assert.commandWorked(coll.insert({_id: 0, a: [{b: [{c: 1}, {c: 2}]}, {b: [{c: 3}, {c: 4}]}]}));
assert.commandWorked(coll.insert({_id: 1, a: [{b: [{c: 5}, {c: 6}]}, {b: [{c: 7}, {c: 8}]}]}));
assert.commandWorked(coll.insert({_id: 2, a: [[{b: {c: 7}}]]}));  // doubly nested array
assert.commandWorked(coll.createIndex({"a.b.c": 1}));

// Test that a $match does NOT move before a $project with nested $map that extracts sub-fields from
// an array path. The $map is not considered a rename because it can change the shape of a document
// when the document contains arrays of arrays.
pipeline = [
        {
          $project: {
              d: {
                  $map: {
                      input: "$a",
                      as: "iterOuter",
                      in : {
                          e: {
                              $map: {
                                  input: "$$iterOuter.b",
                                  as: "iterInner",
                                  in : {f: "$$iterInner.c"}
                              }
                          }
                      }
                  }
              }
          }
        },
        {$match: {"d.e.f": 7}}
    ];
assert.sameMembers(
    [{_id: 1, d: [{e: [{f: 5}, {f: 6}]}, {e: [{f: 7}, {f: 8}]}]}, {_id: 2, d: [{e: [{f: 7}]}]}],
    coll.aggregate(pipeline).toArray());
explain = coll.explain().aggregate(pipeline);
collscan = getAggPlanStage(explain, "COLLSCAN");
assert.neq(null, collscan, tojson(explain));

// Test that a $match does NOT move before a $addFields with nested $map that extracts sub-fields
// from two levels of arrays. The $map is not considered a rename because it can change the shape of
// a document when the document contains arrays of arrays.
pipeline = [
        {
          $addFields: {
              d: {
                  $map: {
                      input: "$a",
                      as: "iterOuter",
                      in : {
                          b: {
                              $map: {
                                  input: "$$iterOuter.b",
                                  as: "iterInner",
                                  in : {c: "$$iterInner.c"}
                              }
                          }
                      }
                  }
              }
          }
        },
        {$match: {"d.b.c": 7}}
    ];
assert.sameMembers(
    [
        {
            _id: 1,
            a: [{b: [{c: 5}, {c: 6}]}, {b: [{c: 7}, {c: 8}]}],
            d: [{b: [{c: 5}, {c: 6}]}, {b: [{c: 7}, {c: 8}]}]
        },
        {_id: 2, a: [[{b: {c: 7}}]], d: [{b: [{c: 7}]}]}
    ],
    coll.aggregate(pipeline).toArray());
explain = coll.explain().aggregate(pipeline);
collscan = getAggPlanStage(explain, "COLLSCAN");
assert.neq(null, collscan, tojson(explain));

// Test that we correctly match on the subfield of a renamed field. Here, a match on "x.b.c"
// follows an "a" to "x" rename. When we move the match stage in front of the rename, the match
// should also get rewritten to use "a.b.c" as its filter.
pipeline = [{$project: {x: "$a"}}, {$match: {"x.b.c": 1}}];
assert.eq([{_id: 0, x: [{b: [{c: 1}, {c: 2}]}, {b: [{c: 3}, {c: 4}]}]}],
          coll.aggregate(pipeline).toArray());
explain = coll.explain().aggregate(pipeline);
let ixscan = getAggPlanStage(explain, "IXSCAN");
assert.neq(null, ixscan, tojson(explain));
assert.eq({"a.b.c": 1}, ixscan.keyPattern, tojson(ixscan));

// Test that we do NOT move a $match on the subfield of a new path created by a $project with $map
// that extracts sub-fields from an array path. The $map is not considered a rename because it can
// change the shape of a document when the document contains arrays of arrays.
pipeline = [
    {$project: {d: {$map: {input: "$a", as: "iter", in : {e: "$$iter.b"}}}}},
    {$match: {"d.e.c": 7}}
];
assert.sameMembers(
    [{_id: 1, d: [{e: [{c: 5}, {c: 6}]}, {e: [{c: 7}, {c: 8}]}]}, {_id: 2, d: [{e: [{c: 7}]}]}],
    coll.aggregate(pipeline).toArray());
explain = coll.explain().aggregate(pipeline);
collscan = getAggPlanStage(explain, "COLLSCAN");
assert.neq(null, collscan, tojson(explain));

// Test multiple renames. Designed to reproduce SERVER-32690.
pipeline = [{$project: {x: "$x", y: "$x"}}, {$match: {y: 1, w: 1}}];
assert.eq([], coll.aggregate(pipeline).toArray());
explain = coll.explain().aggregate(pipelineWithoutOptimizations(pipeline));
// We expect that the $match stage has been split into two, since one predicate has an
// applicable rename that allows swapping, while the other does not.
let matchStages = getAggPlanStages(explain, "$match");
assert.eq(2, matchStages.length);

// Test that we correctly match using the '$elemMatch' expression on renamed subfields. Designed to
// reproduce HELP-59485.
coll.drop();
assert.commandWorked(coll.insertMany([
    {
        _id: 0,
        otherField: "same-string",
        outer: undefined,
    },
    {
        _id: 1,
        otherField: "same-string",
        outer: [{inner: true}],
    },
    {
        _id: 2,
        otherField: "same-string",
        outer: [[], [[{inner: true}]]],
    },
    {
        _id: 3,
        otherField: "same-string",
        outer: [[], [[]], [[[{inner: true}]]]],
    },
    {
        _id: 4,
        otherField: "same-string",
        outer: [{inner: [true]}],
    },
]));

function runElemMatchTest({pipeline, expectedDocumentIds}) {
    const extractDocumentId = ({_id}) => _id;
    const actualIds = coll.aggregate(pipeline).toArray().map(extractDocumentId);
    assert.eq(actualIds, expectedDocumentIds);
    // Expect the '$match' expression to be split into two parts, since the 'otherField' rename is
    // still a valid rewrite.
    const explain = coll.explain().aggregate(pipelineWithoutOptimizations(pipeline));
    const matchStages = getAggPlanStages(explain, "$match");
    assert.eq(2, matchStages.length);
}

runElemMatchTest({
    pipeline: [
        {
            $addFields: {
                flattened: {
                    $map: {input: '$outer', as: "iter", in : "$$iter.inner"},
                },
                renamedOtherField: "$otherField"
            },
        },
        {
            $match: {flattened: {$elemMatch: {$eq: true}}, renamedOtherField: "same-string"},
        }
    ],
    expectedDocumentIds: [1]
});

// Repeat the previous test case, but this time with a $project stage targeting a deeply nested
// transform.
runElemMatchTest({
    pipeline: [
        {
            $project: {
                a: {
                    b: {
                        c: {
                            $map: {input: '$outer', as: "iter", in : "$$iter.inner"},
                        }
                    }
                },
                renamedOtherField: "$otherField"
            }
        },
        {
            $match: {"a.b.c": {$elemMatch: {$eq: true}}, renamedOtherField: "same-string"},
        }
    ],
    expectedDocumentIds: [1],
});

// Similarly, ensure that we match on the correct documents when using $elemMatch expressions on
// simple dot-syntax renamed fields.
runElemMatchTest({
    pipeline: [
        {
            $project: {flattened: "$outer.inner", renamedOtherField: "$otherField"},
        },
        {
            $match: {flattened: {$elemMatch: {$eq: true}}, renamedOtherField: "same-string"},
        }
    ],
    expectedDocumentIds: [1]
});
