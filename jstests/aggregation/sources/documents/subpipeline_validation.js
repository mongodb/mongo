// Tests the validation logic for combinations of "collectionless" stages like $documents with or
// around sub-pipelines. For the cases that should be legal, we mostly just care the the command
// succeeds. However, we will use 'resultsEq' to test correct semantics while we are here, gaining
// more coverage.
// TODO SERVER-94226 consider extending this test to cases like $currentOp and $queryStats as well.
// This test uses stages like $documents which are not permitted inside a $facet stage.
// @tags: [do_not_wrap_aggregations_in_facets]
import {resultsEq} from "jstests/aggregation/extras/utils.js";

const coll = db[jsTestName()];
coll.drop();

const targetCollForMerge = db["target_coll"];
targetCollForMerge.drop();
assert.commandWorked(coll.insert({_id: 0, arr: [{}, {}]}));

{
    // Tests for an aggregation over a collection (i.e. {aggregate: "collName"} commands) with a
    // $documents stage used in a sub-pipeline. Each of these cases should be legal, which is most
    // of the value of the assertion. We will use 'resultsEq' to test correct semantics while we are
    // here.

    // $lookup.
    assert(resultsEq(coll.aggregate([
      {$lookup: {
        let: {documents: "$arr"},
        pipeline: [
          {$documents: "$$documents"},
        ],
        as: "duplicated"
      }},
    ]).toArray(), [{_id: 0, arr: [{}, {}], duplicated: [{}, {}]}]));

    // $unionWith.
    assert(resultsEq(coll.aggregate([
                             {
                                 $unionWith: {
                                     pipeline: [
                                         {$documents: [{_id: "gen"}]},
                                     ],
                                 }
                             },
                         ])
                         .toArray(),
                     [{_id: 0, arr: [{}, {}]}, {_id: "gen"}]));

    // Both, and more nesting.
    assert(resultsEq(coll.aggregate([{
                             $unionWith: {
                              coll: coll.getName(),
                                 pipeline: [{
                                     $lookup: {
                                         pipeline: [
                                             {$documents: []},
                                             {$unionWith: {coll: coll.getName(), pipeline: []}}
                                         ],
                                         as: "nest"
                                     }
                                 }]
                             }
                         }])
                         .toArray(),
                     [
                       {_id: 0, arr: [{}, {}]},
                       {_id: 0, arr: [{}, {}], nest: [{_id: 0, arr: [{}, {}]}]}
                     ]));
}

{
    // Tests for a db-level aggregate (i.e. {aggregate: 1} commands) with sub-pipelines on regular
    // collections.

    // $lookup.
    assert(resultsEq(db.aggregate([
      {$documents: [{x: 1, arr: [{x: 2}]}, {y: 1, arr: []}]},
      {$lookup: {
        let: {documents: "$arr"},
        pipeline: [
          {$documents: "$$documents"},
        ],
        as: "duplicated"
      }},
    ]).toArray(), [{x: 1, arr: [{x: 2}], duplicated: [{x: 2}]}, {y: 1, arr: [], duplicated: []}]));

    // $merge.
    assert.doesNotThrow(() => db.aggregate([
        {
            $documents: [
                {_id: 2, x: "foo"},
                {_id: 4, x: "bar"},
            ]
        },
        {
            $merge: {
                into: targetCollForMerge.getName(),
                on: "_id",
                whenMatched: [{$set: {x: {$setUnion: ["$x", "$$new.x"]}}}]
            }
        }
    ]));
    assert(resultsEq(targetCollForMerge.find({}, {_id: 1}).toArray(), [{_id: 2}, {_id: 4}]));

    // $unionWith
    assert(resultsEq(db.aggregate([
                           {$documents: [{_id: 2}, {_id: 4}]},
                           {$unionWith: {coll: coll.getName(), pipeline: []}}
                       ]).toArray(),
                     [{_id: 2}, {_id: 4}, {_id: 0, arr: [{}, {}]}]));

    // $facet
    assert(resultsEq(db.aggregate([
                           {
                               $documents: [
                                   {x: 1, y: 1, val: 1},
                                   {x: 2, y: 2, val: 1},
                                   {x: 3, y: 1, val: 2},
                                   {x: 2, y: 2, val: 1}
                               ]
                           },
                           {
                               $facet: {
                                   sumByX: [{$group: {_id: "$x", sum: {$sum: "$val"}}}],
                                   sumByY: [{$group: {_id: "$y", sum: {$sum: "$val"}}}]
                               }
                           }
                       ]).toArray(),
                     [{
                         sumByX: [{_id: 1, sum: 1}, {_id: 2, sum: 2}, {_id: 3, sum: 2}],
                         sumByY: [{_id: 1, sum: 3}, {_id: 2, sum: 2}]
                     }]));

    // All of the above, plus nesting.
    const results =
        db.aggregate([
              {$documents: [{_id: "first"}]},
              {
                  $unionWith: {
                      pipeline: [
                          {$documents: [{_id: "uw"}]},
                          {$unionWith: {pipeline: [{$documents: [{_id: "uw_2"}]}]}},
                          {
                              $facet: {
                                  allTogether: [{$group: {_id: null, all: {$addToSet: "$_id"}}}],
                                  countEach: [{$group: {_id: "$_id", count: {$sum: 1}}}],
                              }
                          },
                          {$lookup: {pipeline: [{$documents: [{x: "lu1"}, {x: "lu2"}]}], as: "xs"}},
                          {$set: {xs: {$map: {input: "$xs", in : "$$this.x"}}}}
                      ]
                  },
              },
          ]).toArray();
    assert(resultsEq(results,
                     [
                         {_id: "first"},
                         {
                             allTogether: [{_id: null, all: ["uw", "uw_2"]}],
                             countEach: [{_id: "uw", count: 1}, {_id: "uw_2", count: 1}],
                             xs: ["lu1", "lu2"]
                         }
                     ]),
           results);
}

// Test for invalid combinations.

// To use $documents inside a $lookup, there must not be a "from" argument.
assert.throwsWithCode(
    () => coll.aggregate([{$lookup: {from: "foo", pipeline: [{$documents: []}], as: "lustuff"}}]),
    ErrorCodes.InvalidNamespace);
assert.throwsWithCode(
    () => coll.aggregate([
      {$lookup: {
        from: "foo",
        let: {docs: "$docs"},
        pipeline: [
          {$documents: ["$$docs"]},
          {$lookup: {
            from: "foo",
            let: {x: "$x", y: "$y"},
            pipeline: [
              {$match: {$expr: {$and: [
                {$eq: ["$x", "$$x"]},
                {$eq: ["$y", "$$y"]}
              ]}}}
            ],
            as: "doesnt_matter"
          }}
      ],
      as: "lustuff"
    }}]),
    ErrorCodes.InvalidNamespace);

// To use $documents inside a $unionWith, there must not be a "coll" argument.
assert.throwsWithCode(
    () => coll.aggregate([{$unionWith: {coll: "foo", pipeline: [{$documents: []}]}}]),
    ErrorCodes.InvalidNamespace);

// Cannot use $documents inside of $facet.
assert.throwsWithCode(() => coll.aggregate([{$facet: {test: [{$documents: []}]}}]), 40600);
