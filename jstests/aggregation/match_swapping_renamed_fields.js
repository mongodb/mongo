/**
 * Tests for the $match swapping optimization's ability to handle fields renamed in $project or
 * $addFields.
 * @tags: [do_not_wrap_aggregations_in_facets]
 */
(function() {
    "use strict";

    load("jstests/libs/analyze_plan.js");

    let coll = db.match_swapping_renamed_fields;
    coll.drop();

    assert.writeOK(coll.insert([{a: 1, b: 1, c: 1}, {a: 2, b: 2, c: 2}, {a: 3, b: 3, c: 3}]));
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
    pipeline =
        [{$project: {z: "$a", zz: {$sum: ["$a", "$b"]}}}, {$match: {z: {$gt: 1}, zz: {$lt: 5}}}];
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
    assert.writeOK(coll.insert({_id: 0, a: [{b: 1, c: 1}, {b: 2, c: 2}]}));
    assert.writeOK(coll.insert({_id: 1, a: [{b: 3, c: 3}, {b: 4, c: 4}]}));
    assert.commandWorked(coll.createIndex({"a.b": 1, "a.c": 1}));

    // Test that a $match can result in index usage after moving past a dotted array path renamed by
    // a $map inside a $project.
    pipeline = [
        {$project: {d: {$map: {input: "$a", as: "iter", in : {e: "$$iter.b", f: "$$iter.c"}}}}},
        {$match: {"d.e": 1, "d.f": 2}}
    ];
    assert.eq([{_id: 0, d: [{e: 1, f: 1}, {e: 2, f: 2}]}], coll.aggregate(pipeline).toArray());
    explain = coll.explain().aggregate(pipeline);
    let ixscan = getAggPlanStage(explain, "IXSCAN");
    assert.neq(null, ixscan, tojson(explain));
    assert.eq({"a.b": 1, "a.c": 1}, ixscan.keyPattern, tojson(ixscan));

    // Test that a $match can result in index usage after moving past a dotted array path renamed by
    // a $map inside an $addFields. This time the match expression is partially dependent and should
    // get split.
    pipeline = [
        {
          $addFields:
              {d: {$map: {input: "$a", as: "iter", in : {e: "$$iter.b", f: "$$iter.c"}}}, g: 2}
        },
        {$match: {"d.e": 1, g: 2}}
    ];
    assert.eq([{_id: 0, a: [{b: 1, c: 1}, {b: 2, c: 2}], d: [{e: 1, f: 1}, {e: 2, f: 2}], g: 2}],
              coll.aggregate(pipeline).toArray());
    explain = coll.explain().aggregate(pipeline);
    ixscan = getAggPlanStage(explain, "IXSCAN");
    assert.neq(null, ixscan, tojson(explain));
    assert.eq({"a.b": 1, "a.c": 1}, ixscan.keyPattern, tojson(ixscan));

    // Test that match swapping behaves correctly when a $map contains a rename but also computes a
    // new field.
    pipeline = [
        {
          $addFields:
              {d: {$map: {input: "$a", as: "iter", in : {e: "$$iter.b", f: {$literal: 99}}}}}
        },
        {$match: {"d.e": 1, "d.f": 99}}
    ];
    assert.eq([{_id: 0, a: [{b: 1, c: 1}, {b: 2, c: 2}], d: [{e: 1, f: 99}, {e: 2, f: 99}]}],
              coll.aggregate(pipeline).toArray());
    explain = coll.explain().aggregate(pipeline);
    ixscan = getAggPlanStage(explain, "IXSCAN");
    assert.neq(null, ixscan, tojson(explain));
    assert.eq({"a.b": 1, "a.c": 1}, ixscan.keyPattern, tojson(ixscan));

    coll.drop();
    assert.writeOK(coll.insert({_id: 0, a: [{b: [{c: 1}, {c: 2}]}, {b: [{c: 3}, {c: 4}]}]}));
    assert.writeOK(coll.insert({_id: 1, a: [{b: [{c: 5}, {c: 6}]}, {b: [{c: 7}, {c: 8}]}]}));
    assert.commandWorked(coll.createIndex({"a.b.c": 1}));

    // Test that a $match can result in index usage by moving past a rename of a field inside
    // two-levels of arrays. The rename is expressed using nested $map inside a $project.
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
    assert.eq([{_id: 1, d: [{e: [{f: 5}, {f: 6}]}, {e: [{f: 7}, {f: 8}]}]}],
              coll.aggregate(pipeline).toArray());
    explain = coll.explain().aggregate(pipeline);
    ixscan = getAggPlanStage(explain, "IXSCAN");
    assert.neq(null, ixscan, tojson(explain));
    assert.eq({"a.b.c": 1}, ixscan.keyPattern, tojson(ixscan));

    // Test that a $match can result in index usage by moving past a rename of a field inside
    // two-levels of arrays. The rename is expressed using nested $map inside an $addFields.
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
    assert.eq([{
                 _id: 1,
                 a: [{b: [{c: 5}, {c: 6}]}, {b: [{c: 7}, {c: 8}]}],
                 d: [{b: [{c: 5}, {c: 6}]}, {b: [{c: 7}, {c: 8}]}]
              }],
              coll.aggregate(pipeline).toArray());
    explain = coll.explain().aggregate(pipeline);
    ixscan = getAggPlanStage(explain, "IXSCAN");
    assert.neq(null, ixscan, tojson(explain));
    assert.eq({"a.b.c": 1}, ixscan.keyPattern, tojson(ixscan));

    // Test that we correctly match on the subfield of a renamed field. Here, a match on "x.b.c"
    // follows an "a" to "x" rename. When we move the match stage in front of the rename, the match
    // should also get rewritten to use "a.b.c" as its filter.
    pipeline = [{$project: {x: "$a"}}, {$match: {"x.b.c": 1}}];
    assert.eq([{_id: 0, x: [{b: [{c: 1}, {c: 2}]}, {b: [{c: 3}, {c: 4}]}]}],
              coll.aggregate(pipeline).toArray());
    explain = coll.explain().aggregate(pipeline);
    ixscan = getAggPlanStage(explain, "IXSCAN");
    assert.neq(null, ixscan, tojson(explain));
    assert.eq({"a.b.c": 1}, ixscan.keyPattern, tojson(ixscan));

    // Test that we correctly match on the subfield of a renamed field when the rename results from
    // a $map operation. Here, a match on "d.e.c" follows an "a.b" to "d.e" rename. When we move the
    // match stage in front of the renaming $map operation, the match should also get rewritten to
    // use "a.b.c" as its filter.
    pipeline = [
        {$project: {d: {$map: {input: "$a", as: "iter", in : {e: "$$iter.b"}}}}},
        {$match: {"d.e.c": 7}}
    ];
    assert.eq([{_id: 1, d: [{e: [{c: 5}, {c: 6}]}, {e: [{c: 7}, {c: 8}]}]}],
              coll.aggregate(pipeline).toArray());
    explain = coll.explain().aggregate(pipeline);
    ixscan = getAggPlanStage(explain, "IXSCAN");
    assert.neq(null, ixscan, tojson(explain));
    assert.eq({"a.b.c": 1}, ixscan.keyPattern, tojson(ixscan));

    // Test multiple renames. Designed to reproduce SERVER-32690.
    pipeline = [
        {$_internalInhibitOptimization: {}},
        {$project: {x: "$x", y: "$x"}},
        {$match: {y: 1, w: 1}}
    ];
    assert.eq([], coll.aggregate(pipeline).toArray());
    explain = coll.explain().aggregate(pipeline);
    // We expect that the $match stage has been split into two, since one predicate has an
    // applicable rename that allows swapping, while the other does not.
    let matchStages = getAggPlanStages(explain, "$match");
    assert.eq(2, matchStages.length);
}());
